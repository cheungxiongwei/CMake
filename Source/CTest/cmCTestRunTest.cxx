/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#ifdef _WIN32
/* windows.h defines min() and max() macros that interfere. */
#define NOMINMAX
#endif
#include "cmCTestRunTest.h"

#include "cmCTest.h"
#include "cmCTestMemCheckHandler.h"
#include "cmCTestTestHandler.h"
#include "cmProcess.h"
#include "cmSystemTools.h"
#include "cmWorkingDirectory.h"

#include "cm_curl.h"
#include "cm_zlib.h"
#include "cmsys/Base64.h"
#include "cmsys/Process.h"
#include "cmsys/RegularExpression.hxx"
#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdio.h>
#include <time.h>
#include <utility>

cmCTestRunTest::cmCTestRunTest(cmCTestTestHandler* handler)
{
  this->CTest = handler->CTest;
  this->TestHandler = handler;
  this->TestProcess = nullptr;
  this->TestResult.ExecutionTime = std::chrono::duration<double>::zero();
  this->TestResult.ReturnValue = 0;
  this->TestResult.Status = cmCTestTestHandler::NOT_RUN;
  this->TestResult.TestCount = 0;
  this->TestResult.Properties = nullptr;
  this->ProcessOutput.clear();
  this->CompressedOutput.clear();
  this->CompressionRatio = 2;
  this->StopTimePassed = false;
  this->NumberOfRunsLeft = 1; // default to 1 run of the test
  this->RunUntilFail = false; // default to run the test once
  this->RunAgain = false;     // default to not having to run again
}

cmCTestRunTest::~cmCTestRunTest()
{
}

bool cmCTestRunTest::CheckOutput()
{
  // Read lines for up to 0.1 seconds of total time.
  std::chrono::duration<double> timeout = std::chrono::milliseconds(100);
  auto timeEnd = std::chrono::steady_clock::now() + timeout;
  std::string line;
  while ((timeout = timeEnd - std::chrono::steady_clock::now(),
          timeout > std::chrono::seconds(0))) {
    int p = this->TestProcess->GetNextOutputLine(line, timeout);
    if (p == cmsysProcess_Pipe_None) {
      // Process has terminated and all output read.
      return false;
    }
    if (p == cmsysProcess_Pipe_STDOUT) {
      // Store this line of output.
      cmCTestLog(this->CTest, HANDLER_VERBOSE_OUTPUT, this->GetIndex()
                   << ": " << line << std::endl);
      this->ProcessOutput += line;
      this->ProcessOutput += "\n";

      // Check for TIMEOUT_AFTER_MATCH property.
      if (!this->TestProperties->TimeoutRegularExpressions.empty()) {
        for (auto& reg : this->TestProperties->TimeoutRegularExpressions) {
          if (reg.first.find(this->ProcessOutput.c_str())) {
            cmCTestLog(this->CTest, HANDLER_VERBOSE_OUTPUT, this->GetIndex()
                         << ": "
                         << "Test timeout changed to "
                         << std::chrono::duration_cast<std::chrono::seconds>(
                              this->TestProperties->AlternateTimeout)
                              .count()
                         << std::endl);
            this->TestProcess->ResetStartTime();
            this->TestProcess->ChangeTimeout(
              this->TestProperties->AlternateTimeout);
            this->TestProperties->TimeoutRegularExpressions.clear();
            break;
          }
        }
      }
    } else { // if(p == cmsysProcess_Pipe_Timeout)
      break;
    }
  }
  return true;
}

// Streamed compression of test output.  The compressed data
// is appended to this->CompressedOutput
void cmCTestRunTest::CompressOutput()
{
  int ret;
  z_stream strm;

  unsigned char* in = reinterpret_cast<unsigned char*>(
    const_cast<char*>(this->ProcessOutput.c_str()));
  // zlib makes the guarantee that this is the maximum output size
  int outSize = static_cast<int>(
    static_cast<double>(this->ProcessOutput.size()) * 1.001 + 13.0);
  unsigned char* out = new unsigned char[outSize];

  strm.zalloc = Z_NULL;
  strm.zfree = Z_NULL;
  strm.opaque = Z_NULL;
  ret = deflateInit(&strm, -1); // default compression level
  if (ret != Z_OK) {
    delete[] out;
    return;
  }

  strm.avail_in = static_cast<uInt>(this->ProcessOutput.size());
  strm.next_in = in;
  strm.avail_out = outSize;
  strm.next_out = out;
  ret = deflate(&strm, Z_FINISH);

  if (ret != Z_STREAM_END) {
    cmCTestLog(this->CTest, ERROR_MESSAGE,
               "Error during output compression. Sending uncompressed output."
                 << std::endl);
    delete[] out;
    return;
  }

  (void)deflateEnd(&strm);

  unsigned char* encoded_buffer =
    new unsigned char[static_cast<int>(outSize * 1.5)];

  size_t rlen = cmsysBase64_Encode(out, strm.total_out, encoded_buffer, 1);

  this->CompressedOutput.clear();
  for (size_t i = 0; i < rlen; i++) {
    this->CompressedOutput += encoded_buffer[i];
  }

  if (strm.total_in) {
    this->CompressionRatio =
      static_cast<double>(strm.total_out) / static_cast<double>(strm.total_in);
  }

  delete[] encoded_buffer;
  delete[] out;
}

bool cmCTestRunTest::EndTest(size_t completed, size_t total, bool started)
{
  if ((!this->TestHandler->MemCheck &&
       this->CTest->ShouldCompressTestOutput()) ||
      (this->TestHandler->MemCheck &&
       this->CTest->ShouldCompressTestOutput())) {
    this->CompressOutput();
  }

  this->WriteLogOutputTop(completed, total);
  std::string reason;
  bool passed = true;
  int res =
    started ? this->TestProcess->GetProcessStatus() : cmsysProcess_State_Error;
  int retVal = this->TestProcess->GetExitValue();
  bool forceFail = false;
  bool skipped = false;
  bool outputTestErrorsToConsole = false;
  if (!this->TestProperties->RequiredRegularExpressions.empty() &&
      this->FailedDependencies.empty()) {
    bool found = false;
    for (auto& pass : this->TestProperties->RequiredRegularExpressions) {
      if (pass.first.find(this->ProcessOutput.c_str())) {
        found = true;
        reason = "Required regular expression found.";
        break;
      }
    }
    if (!found) {
      reason = "Required regular expression not found.";
      forceFail = true;
    }
    reason += "Regex=[";
    for (auto& pass : this->TestProperties->RequiredRegularExpressions) {
      reason += pass.second;
      reason += "\n";
    }
    reason += "]";
  }
  if (!this->TestProperties->ErrorRegularExpressions.empty() &&
      this->FailedDependencies.empty()) {
    for (auto& pass : this->TestProperties->ErrorRegularExpressions) {
      if (pass.first.find(this->ProcessOutput.c_str())) {
        reason = "Error regular expression found in output.";
        reason += " Regex=[";
        reason += pass.second;
        reason += "]";
        forceFail = true;
        break;
      }
    }
  }
  if (res == cmsysProcess_State_Exited) {
    bool success = !forceFail &&
      (retVal == 0 ||
       !this->TestProperties->RequiredRegularExpressions.empty());
    if (this->TestProperties->SkipReturnCode >= 0 &&
        this->TestProperties->SkipReturnCode == retVal) {
      this->TestResult.Status = cmCTestTestHandler::NOT_RUN;
      std::ostringstream s;
      s << "SKIP_RETURN_CODE=" << this->TestProperties->SkipReturnCode;
      this->TestResult.CompletionStatus = s.str();
      cmCTestLog(this->CTest, HANDLER_OUTPUT, "***Skipped ");
      skipped = true;
    } else if ((success && !this->TestProperties->WillFail) ||
               (!success && this->TestProperties->WillFail)) {
      this->TestResult.Status = cmCTestTestHandler::COMPLETED;
      cmCTestLog(this->CTest, HANDLER_OUTPUT, "   Passed  ");
    } else {
      this->TestResult.Status = cmCTestTestHandler::FAILED;
      cmCTestLog(this->CTest, HANDLER_OUTPUT, "***Failed  " << reason);
      outputTestErrorsToConsole = this->CTest->OutputTestOutputOnTestFailure;
    }
  } else if (res == cmsysProcess_State_Expired) {
    cmCTestLog(this->CTest, HANDLER_OUTPUT, "***Timeout ");
    this->TestResult.Status = cmCTestTestHandler::TIMEOUT;
    outputTestErrorsToConsole = this->CTest->OutputTestOutputOnTestFailure;
  } else if (res == cmsysProcess_State_Exception) {
    outputTestErrorsToConsole = this->CTest->OutputTestOutputOnTestFailure;
    cmCTestLog(this->CTest, HANDLER_OUTPUT, "***Exception: ");
    this->TestResult.ExceptionStatus =
      this->TestProcess->GetExitExceptionString();
    switch (this->TestProcess->GetExitException()) {
      case cmsysProcess_Exception_Fault:
        cmCTestLog(this->CTest, HANDLER_OUTPUT, "SegFault");
        this->TestResult.Status = cmCTestTestHandler::SEGFAULT;
        break;
      case cmsysProcess_Exception_Illegal:
        cmCTestLog(this->CTest, HANDLER_OUTPUT, "Illegal");
        this->TestResult.Status = cmCTestTestHandler::ILLEGAL;
        break;
      case cmsysProcess_Exception_Interrupt:
        cmCTestLog(this->CTest, HANDLER_OUTPUT, "Interrupt");
        this->TestResult.Status = cmCTestTestHandler::INTERRUPT;
        break;
      case cmsysProcess_Exception_Numerical:
        cmCTestLog(this->CTest, HANDLER_OUTPUT, "Numerical");
        this->TestResult.Status = cmCTestTestHandler::NUMERICAL;
        break;
      default:
        cmCTestLog(this->CTest, HANDLER_OUTPUT,
                   this->TestResult.ExceptionStatus);
        this->TestResult.Status = cmCTestTestHandler::OTHER_FAULT;
    }
  } else if ("Disabled" == this->TestResult.CompletionStatus) {
    cmCTestLog(this->CTest, HANDLER_OUTPUT, "***Not Run (Disabled) ");
  } else // cmsysProcess_State_Error
  {
    cmCTestLog(this->CTest, HANDLER_OUTPUT, "***Not Run ");
  }

  passed = this->TestResult.Status == cmCTestTestHandler::COMPLETED;
  char buf[1024];
  sprintf(buf, "%6.2f sec",
          double(std::chrono::duration_cast<std::chrono::milliseconds>(
                   this->TestProcess->GetTotalTime())
                   .count()) /
            1000.0);
  cmCTestLog(this->CTest, HANDLER_OUTPUT, buf << "\n");

  if (outputTestErrorsToConsole) {
    cmCTestLog(this->CTest, HANDLER_OUTPUT, this->ProcessOutput << std::endl);
  }

  if (this->TestHandler->LogFile) {
    *this->TestHandler->LogFile << "Test time = " << buf << std::endl;
  }

  // Set the working directory to the tests directory to process Dart files.
  {
    cmWorkingDirectory workdir(this->TestProperties->Directory);
    this->DartProcessing();
  }

  // if this is doing MemCheck then all the output needs to be put into
  // Output since that is what is parsed by cmCTestMemCheckHandler
  if (!this->TestHandler->MemCheck && started) {
    this->TestHandler->CleanTestOutput(
      this->ProcessOutput,
      static_cast<size_t>(
        this->TestResult.Status == cmCTestTestHandler::COMPLETED
          ? this->TestHandler->CustomMaximumPassedTestOutputSize
          : this->TestHandler->CustomMaximumFailedTestOutputSize));
  }
  this->TestResult.Reason = reason;
  if (this->TestHandler->LogFile) {
    bool pass = true;
    const char* reasonType = "Test Pass Reason";
    if (this->TestResult.Status != cmCTestTestHandler::COMPLETED &&
        this->TestResult.Status != cmCTestTestHandler::NOT_RUN) {
      reasonType = "Test Fail Reason";
      pass = false;
    }
    auto ttime = this->TestProcess->GetTotalTime();
    auto hours = std::chrono::duration_cast<std::chrono::hours>(ttime);
    ttime -= hours;
    auto minutes = std::chrono::duration_cast<std::chrono::minutes>(ttime);
    ttime -= minutes;
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(ttime);
    char buffer[100];
    sprintf(buffer, "%02d:%02d:%02d", static_cast<unsigned>(hours.count()),
            static_cast<unsigned>(minutes.count()),
            static_cast<unsigned>(seconds.count()));
    *this->TestHandler->LogFile
      << "----------------------------------------------------------"
      << std::endl;
    if (!this->TestResult.Reason.empty()) {
      *this->TestHandler->LogFile << reasonType << ":\n"
                                  << this->TestResult.Reason << "\n";
    } else {
      if (pass) {
        *this->TestHandler->LogFile << "Test Passed.\n";
      } else {
        *this->TestHandler->LogFile << "Test Failed.\n";
      }
    }
    *this->TestHandler->LogFile
      << "\"" << this->TestProperties->Name
      << "\" end time: " << this->CTest->CurrentTime() << std::endl
      << "\"" << this->TestProperties->Name << "\" time elapsed: " << buffer
      << std::endl
      << "----------------------------------------------------------"
      << std::endl
      << std::endl;
  }
  // if the test actually started and ran
  // record the results in TestResult
  if (started) {
    bool compress = !this->TestHandler->MemCheck &&
      this->CompressionRatio < 1 && this->CTest->ShouldCompressTestOutput();
    this->TestResult.Output =
      compress ? this->CompressedOutput : this->ProcessOutput;
    this->TestResult.CompressOutput = compress;
    this->TestResult.ReturnValue = this->TestProcess->GetExitValue();
    if (!skipped) {
      this->TestResult.CompletionStatus = "Completed";
    }
    this->TestResult.ExecutionTime = this->TestProcess->GetTotalTime();
    this->MemCheckPostProcess();
    this->ComputeWeightedCost();
  }
  // If the test does not need to rerun push the current TestResult onto the
  // TestHandler vector
  if (!this->NeedsToRerun()) {
    this->TestHandler->TestResults.push_back(this->TestResult);
  }
  delete this->TestProcess;
  return passed || skipped;
}

bool cmCTestRunTest::StartAgain()
{
  if (!this->RunAgain) {
    return false;
  }
  this->RunAgain = false; // reset
  // change to tests directory
  cmWorkingDirectory workdir(this->TestProperties->Directory);
  this->StartTest(this->TotalNumberOfTests);
  return true;
}

bool cmCTestRunTest::NeedsToRerun()
{
  this->NumberOfRunsLeft--;
  if (this->NumberOfRunsLeft == 0) {
    return false;
  }
  // if number of runs left is not 0, and we are running until
  // we find a failed test, then return true so the test can be
  // restarted
  if (this->RunUntilFail &&
      this->TestResult.Status == cmCTestTestHandler::COMPLETED) {
    this->RunAgain = true;
    return true;
  }
  return false;
}
void cmCTestRunTest::ComputeWeightedCost()
{
  double prev = static_cast<double>(this->TestProperties->PreviousRuns);
  double avgcost = static_cast<double>(this->TestProperties->Cost);
  double current =
    double(std::chrono::duration_cast<std::chrono::milliseconds>(
             this->TestResult.ExecutionTime)
             .count()) /
    1000.0;

  if (this->TestResult.Status == cmCTestTestHandler::COMPLETED) {
    this->TestProperties->Cost =
      static_cast<float>(((prev * avgcost) + current) / (prev + 1.0));
    this->TestProperties->PreviousRuns++;
  }
}

void cmCTestRunTest::MemCheckPostProcess()
{
  if (!this->TestHandler->MemCheck) {
    return;
  }
  cmCTestOptionalLog(this->CTest, HANDLER_VERBOSE_OUTPUT, this->Index
                       << ": process test output now: "
                       << this->TestProperties->Name << " "
                       << this->TestResult.Name << std::endl,
                     this->TestHandler->GetQuiet());
  cmCTestMemCheckHandler* handler =
    static_cast<cmCTestMemCheckHandler*>(this->TestHandler);
  handler->PostProcessTest(this->TestResult, this->Index);
}

// Starts the execution of a test.  Returns once it has started
bool cmCTestRunTest::StartTest(size_t total)
{
  this->TotalNumberOfTests = total; // save for rerun case
  cmCTestLog(this->CTest, HANDLER_OUTPUT, std::setw(2 * getNumWidth(total) + 8)
               << "Start "
               << std::setw(getNumWidth(this->TestHandler->GetMaxIndex()))
               << this->TestProperties->Index << ": "
               << this->TestProperties->Name << std::endl);
  this->ProcessOutput.clear();

  // Return immediately if test is disabled
  if (this->TestProperties->Disabled) {
    this->TestResult.Properties = this->TestProperties;
    this->TestResult.ExecutionTime = std::chrono::duration<double>::zero();
    this->TestResult.CompressOutput = false;
    this->TestResult.ReturnValue = -1;
    this->TestResult.CompletionStatus = "Disabled";
    this->TestResult.Status = cmCTestTestHandler::NOT_RUN;
    this->TestResult.TestCount = this->TestProperties->Index;
    this->TestResult.Name = this->TestProperties->Name;
    this->TestResult.Path = this->TestProperties->Directory;
    this->TestProcess = new cmProcess;
    this->TestResult.Output = "Disabled";
    this->TestResult.FullCommandLine.clear();
    return false;
  }

  this->ComputeArguments();
  std::vector<std::string>& args = this->TestProperties->Args;
  this->TestResult.Properties = this->TestProperties;
  this->TestResult.ExecutionTime = std::chrono::duration<double>::zero();
  this->TestResult.CompressOutput = false;
  this->TestResult.ReturnValue = -1;
  this->TestResult.CompletionStatus = "Failed to start";
  this->TestResult.Status = cmCTestTestHandler::BAD_COMMAND;
  this->TestResult.TestCount = this->TestProperties->Index;
  this->TestResult.Name = this->TestProperties->Name;
  this->TestResult.Path = this->TestProperties->Directory;

  if (!this->FailedDependencies.empty()) {
    this->TestProcess = new cmProcess;
    std::string msg = "Failed test dependencies:";
    for (std::string const& failedDep : this->FailedDependencies) {
      msg += " " + failedDep;
    }
    *this->TestHandler->LogFile << msg << std::endl;
    cmCTestLog(this->CTest, HANDLER_OUTPUT, msg << std::endl);
    this->TestResult.Output = msg;
    this->TestResult.FullCommandLine.clear();
    this->TestResult.CompletionStatus = "Fixture dependency failed";
    this->TestResult.Status = cmCTestTestHandler::NOT_RUN;
    return false;
  }

  if (args.size() >= 2 && args[1] == "NOT_AVAILABLE") {
    this->TestProcess = new cmProcess;
    std::string msg;
    if (this->CTest->GetConfigType().empty()) {
      msg = "Test not available without configuration.";
      msg += "  (Missing \"-C <config>\"?)";
    } else {
      msg = "Test not available in configuration \"";
      msg += this->CTest->GetConfigType();
      msg += "\".";
    }
    *this->TestHandler->LogFile << msg << std::endl;
    cmCTestLog(this->CTest, ERROR_MESSAGE, msg << std::endl);
    this->TestResult.Output = msg;
    this->TestResult.FullCommandLine.clear();
    this->TestResult.CompletionStatus = "Missing Configuration";
    this->TestResult.Status = cmCTestTestHandler::NOT_RUN;
    return false;
  }

  // Check if all required files exist
  for (std::string const& file : this->TestProperties->RequiredFiles) {
    if (!cmSystemTools::FileExists(file.c_str())) {
      // Required file was not found
      this->TestProcess = new cmProcess;
      *this->TestHandler->LogFile << "Unable to find required file: " << file
                                  << std::endl;
      cmCTestLog(this->CTest, ERROR_MESSAGE,
                 "Unable to find required file: " << file << std::endl);
      this->TestResult.Output = "Unable to find required file: " + file;
      this->TestResult.FullCommandLine.clear();
      this->TestResult.CompletionStatus = "Required Files Missing";
      this->TestResult.Status = cmCTestTestHandler::NOT_RUN;
      return false;
    }
  }
  // log and return if we did not find the executable
  if (this->ActualCommand.empty()) {
    // if the command was not found create a TestResult object
    // that has that information
    this->TestProcess = new cmProcess;
    *this->TestHandler->LogFile << "Unable to find executable: " << args[1]
                                << std::endl;
    cmCTestLog(this->CTest, ERROR_MESSAGE,
               "Unable to find executable: " << args[1] << std::endl);
    this->TestResult.Output = "Unable to find executable: " + args[1];
    this->TestResult.FullCommandLine.clear();
    this->TestResult.CompletionStatus = "Unable to find executable";
    this->TestResult.Status = cmCTestTestHandler::NOT_RUN;
    return false;
  }
  this->StartTime = this->CTest->CurrentTime();

  auto timeout = this->ResolveTimeout();

  if (this->StopTimePassed) {
    return false;
  }
  return this->ForkProcess(timeout, this->TestProperties->ExplicitTimeout,
                           &this->TestProperties->Environment);
}

void cmCTestRunTest::ComputeArguments()
{
  this->Arguments.clear(); // reset becaue this might be a rerun
  std::vector<std::string>::const_iterator j =
    this->TestProperties->Args.begin();
  ++j; // skip test name
  // find the test executable
  if (this->TestHandler->MemCheck) {
    cmCTestMemCheckHandler* handler =
      static_cast<cmCTestMemCheckHandler*>(this->TestHandler);
    this->ActualCommand = handler->MemoryTester;
    this->TestProperties->Args[1] = this->TestHandler->FindTheExecutable(
      this->TestProperties->Args[1].c_str());
  } else {
    this->ActualCommand = this->TestHandler->FindTheExecutable(
      this->TestProperties->Args[1].c_str());
    ++j; // skip the executable (it will be actualCommand)
  }
  std::string testCommand =
    cmSystemTools::ConvertToOutputPath(this->ActualCommand.c_str());

  // Prepends memcheck args to our command string
  this->TestHandler->GenerateTestCommand(this->Arguments, this->Index);
  for (std::string const& arg : this->Arguments) {
    testCommand += " \"";
    testCommand += arg;
    testCommand += "\"";
  }

  for (; j != this->TestProperties->Args.end(); ++j) {
    testCommand += " \"";
    testCommand += *j;
    testCommand += "\"";
    this->Arguments.push_back(*j);
  }
  this->TestResult.FullCommandLine = testCommand;

  // Print the test command in verbose mode
  cmCTestLog(this->CTest, HANDLER_VERBOSE_OUTPUT, std::endl
               << this->Index << ": "
               << (this->TestHandler->MemCheck ? "MemCheck" : "Test")
               << " command: " << testCommand << std::endl);

  // Print any test-specific env vars in verbose mode
  if (!this->TestProperties->Environment.empty()) {
    cmCTestLog(this->CTest, HANDLER_VERBOSE_OUTPUT, this->Index
                 << ": "
                 << "Environment variables: " << std::endl);
  }
  for (std::string const& env : this->TestProperties->Environment) {
    cmCTestLog(this->CTest, HANDLER_VERBOSE_OUTPUT, this->Index << ":  " << env
                                                                << std::endl);
  }
}

void cmCTestRunTest::DartProcessing()
{
  if (!this->ProcessOutput.empty() &&
      this->ProcessOutput.find("<DartMeasurement") != std::string::npos) {
    if (this->TestHandler->DartStuff.find(this->ProcessOutput.c_str())) {
      this->TestResult.DartString = this->TestHandler->DartStuff.match(1);
      // keep searching and replacing until none are left
      while (this->TestHandler->DartStuff1.find(this->ProcessOutput.c_str())) {
        // replace the exact match for the string
        cmSystemTools::ReplaceString(
          this->ProcessOutput, this->TestHandler->DartStuff1.match(1).c_str(),
          "");
      }
    }
  }
}

std::chrono::duration<double> cmCTestRunTest::ResolveTimeout()
{
  auto timeout = this->TestProperties->Timeout;

  if (this->CTest->GetStopTime().empty()) {
    return timeout;
  }
  struct tm* lctime;
  time_t current_time = time(nullptr);
  lctime = gmtime(&current_time);
  int gm_hour = lctime->tm_hour;
  time_t gm_time = mktime(lctime);
  lctime = localtime(&current_time);
  int local_hour = lctime->tm_hour;

  int tzone_offset = local_hour - gm_hour;
  if (gm_time > current_time && gm_hour < local_hour) {
    // this means gm_time is on the next day
    tzone_offset -= 24;
  } else if (gm_time < current_time && gm_hour > local_hour) {
    // this means gm_time is on the previous day
    tzone_offset += 24;
  }

  tzone_offset *= 100;
  char buf[1024];
  // add todays year day and month to the time in str because
  // curl_getdate no longer assumes the day is today
  sprintf(buf, "%d%02d%02d %s %+05i", lctime->tm_year + 1900,
          lctime->tm_mon + 1, lctime->tm_mday,
          this->CTest->GetStopTime().c_str(), tzone_offset);

  time_t stop_time_t = curl_getdate(buf, &current_time);
  if (stop_time_t == -1) {
    return timeout;
  }

  auto stop_time = std::chrono::system_clock::from_time_t(stop_time_t);

  // the stop time refers to the next day
  if (this->CTest->NextDayStopTime) {
    stop_time += std::chrono::hours(24);
  }
  auto stop_timeout =
    (stop_time - std::chrono::system_clock::from_time_t(current_time)) %
    std::chrono::hours(24);
  this->CTest->LastStopTimeout = stop_timeout;

  if (stop_timeout <= std::chrono::duration<double>::zero() ||
      stop_timeout > this->CTest->LastStopTimeout) {
    cmCTestLog(this->CTest, ERROR_MESSAGE, "The stop time has been passed. "
                                           "Stopping all tests."
                 << std::endl);
    this->StopTimePassed = true;
    return std::chrono::duration<double>::zero();
  }
  return timeout == std::chrono::duration<double>::zero()
    ? stop_timeout
    : (timeout < stop_timeout ? timeout : stop_timeout);
}

bool cmCTestRunTest::ForkProcess(std::chrono::duration<double> testTimeOut,
                                 bool explicitTimeout,
                                 std::vector<std::string>* environment)
{
  this->TestProcess = new cmProcess;
  this->TestProcess->SetId(this->Index);
  this->TestProcess->SetWorkingDirectory(
    this->TestProperties->Directory.c_str());
  this->TestProcess->SetCommand(this->ActualCommand.c_str());
  this->TestProcess->SetCommandArguments(this->Arguments);

  // determine how much time we have
  std::chrono::duration<double> timeout =
    this->CTest->GetRemainingTimeAllowed();
  if (timeout != std::chrono::duration<double>::max()) {
    timeout -= std::chrono::minutes(2);
  }
  if (this->CTest->GetTimeOut() > std::chrono::duration<double>::zero() &&
      this->CTest->GetTimeOut() < timeout) {
    timeout = this->CTest->GetTimeOut();
  }
  if (testTimeOut > std::chrono::duration<double>::zero() &&
      testTimeOut < this->CTest->GetRemainingTimeAllowed()) {
    timeout = testTimeOut;
  }
  // always have at least 1 second if we got to here
  if (timeout <= std::chrono::duration<double>::zero()) {
    timeout = std::chrono::seconds(1);
  }
  // handle timeout explicitly set to 0
  if (testTimeOut == std::chrono::duration<double>::zero() &&
      explicitTimeout) {
    timeout = std::chrono::duration<double>::zero();
  }
  cmCTestOptionalLog(
    this->CTest, HANDLER_VERBOSE_OUTPUT, this->Index
      << ": "
      << "Test timeout computed to be: "
      << (timeout == std::chrono::duration<double>::max()
            ? std::string("infinite")
            : std::to_string(
                std::chrono::duration_cast<std::chrono::seconds>(timeout)
                  .count()))
      << "\n",
    this->TestHandler->GetQuiet());

  this->TestProcess->SetTimeout(timeout);

#ifdef CMAKE_BUILD_WITH_CMAKE
  cmSystemTools::SaveRestoreEnvironment sre;
#endif

  if (environment && !environment->empty()) {
    cmSystemTools::AppendEnv(*environment);
  }

  return this->TestProcess->StartProcess();
}

void cmCTestRunTest::WriteLogOutputTop(size_t completed, size_t total)
{
  // if this is the last or only run of this test
  // then print out completed / total
  // Only issue is if a test fails and we are running until fail
  // then it will never print out the completed / total, same would
  // got for run until pass.  Trick is when this is called we don't
  // yet know if we are passing or failing.
  if (this->NumberOfRunsLeft == 1) {
    cmCTestLog(this->CTest, HANDLER_OUTPUT, std::setw(getNumWidth(total))
                 << completed << "/");
    cmCTestLog(this->CTest, HANDLER_OUTPUT, std::setw(getNumWidth(total))
                 << total << " ");
  }
  // if this is one of several runs of a test just print blank space
  // to keep things neat
  else {
    cmCTestLog(this->CTest, HANDLER_OUTPUT, std::setw(getNumWidth(total))
                 << " "
                 << " ");
    cmCTestLog(this->CTest, HANDLER_OUTPUT, std::setw(getNumWidth(total))
                 << " "
                 << " ");
  }

  if (this->TestHandler->MemCheck) {
    cmCTestLog(this->CTest, HANDLER_OUTPUT, "MemCheck");
  } else {
    cmCTestLog(this->CTest, HANDLER_OUTPUT, "Test");
  }

  std::ostringstream indexStr;
  indexStr << " #" << this->Index << ":";
  cmCTestLog(this->CTest, HANDLER_OUTPUT,
             std::setw(3 + getNumWidth(this->TestHandler->GetMaxIndex()))
               << indexStr.str());
  cmCTestLog(this->CTest, HANDLER_OUTPUT, " ");
  const int maxTestNameWidth = this->CTest->GetMaxTestNameWidth();
  std::string outname = this->TestProperties->Name + " ";
  outname.resize(maxTestNameWidth + 4, '.');

  *this->TestHandler->LogFile << this->TestProperties->Index << "/"
                              << this->TestHandler->TotalNumberOfTests
                              << " Testing: " << this->TestProperties->Name
                              << std::endl;
  *this->TestHandler->LogFile << this->TestProperties->Index << "/"
                              << this->TestHandler->TotalNumberOfTests
                              << " Test: " << this->TestProperties->Name
                              << std::endl;
  *this->TestHandler->LogFile << "Command: \"" << this->ActualCommand << "\"";

  for (std::string const& arg : this->Arguments) {
    *this->TestHandler->LogFile << " \"" << arg << "\"";
  }
  *this->TestHandler->LogFile
    << std::endl
    << "Directory: " << this->TestProperties->Directory << std::endl
    << "\"" << this->TestProperties->Name
    << "\" start time: " << this->StartTime << std::endl;

  *this->TestHandler->LogFile
    << "Output:" << std::endl
    << "----------------------------------------------------------"
    << std::endl;
  *this->TestHandler->LogFile << this->ProcessOutput << "<end of output>"
                              << std::endl;

  cmCTestLog(this->CTest, HANDLER_OUTPUT, outname.c_str());
  cmCTestLog(this->CTest, DEBUG, "Testing " << this->TestProperties->Name
                                            << " ... ");
}
