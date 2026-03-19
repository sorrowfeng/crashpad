// Copyright 2018 The Crashpad Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "handler/linux/crash_report_exception_handler.h"

#include <memory>
#include <utility>
#include <string>
#include <unistd.h>
#include <cstdlib>

#include "base/logging.h"
#include "build/build_config.h"
#include "client/settings.h"
#include "handler/linux/capture_snapshot.h"
#include "minidump/minidump_file_writer.h"
#include "snapshot/linux/process_snapshot_linux.h"
#include "snapshot/sanitized/process_snapshot_sanitized.h"
#include "util/backtrace/crash_loop_detection.h"
#include "util/file/file_helper.h"
#include "util/file/file_reader.h"
#include "util/file/output_stream_file_writer.h"
#include "util/linux/direct_ptrace_connection.h"
#include "util/linux/ptrace_client.h"
#include "util/misc/implicit_cast.h"
#include "util/misc/metrics.h"
#include "util/misc/uuid.h"
#include "util/stream/base94_output_stream.h"
#include "util/stream/log_output_stream.h"
#include "util/stream/zlib_output_stream.h"

#if BUILDFLAG(IS_ANDROID)
#include <android/log.h>
#endif

namespace crashpad {
namespace {

class Logger final : public LogOutputStream::Delegate {
 public:
  Logger() = default;

  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  ~Logger() override = default;

#if BUILDFLAG(IS_ANDROID)
  int Log(const char* buf) override {
    return __android_log_buf_write(
        LOG_ID_CRASH, ANDROID_LOG_FATAL, "crashpad", buf);
  }

  size_t OutputCap() override {
    // Most minidumps are expected to be compressed and encoded into less than
    // 128k.
    return 128 * 1024;
  }

  size_t LineWidth() override {
    // From Android NDK r20 <android/log.h>, log message text may be truncated
    // to less than an implementation-specific limit (1023 bytes), for sake of
    // safe and being easy to read in logcat, choose 512.
    return 512;
  }
#else
  // TODO(jperaza): Log to an appropriate location on Linux.
  int Log(const char* buf) override { return -ENOTCONN; }
  size_t OutputCap() override { return 0; }
  size_t LineWidth() override { return 0; }
#endif
};

bool WriteMinidumpLogFromFile(FileReaderInterface* file_reader) {
  ZlibOutputStream stream(
      ZlibOutputStream::Mode::kCompress,
      std::make_unique<Base94OutputStream>(
          Base94OutputStream::Mode::kEncode,
          std::make_unique<LogOutputStream>(std::make_unique<Logger>())));
  FileOperationResult read_result;
  do {
    uint8_t buffer[4096];
    read_result = file_reader->Read(buffer, sizeof(buffer));
    if (read_result < 0)
      return false;

    if (read_result > 0 && (!stream.Write(buffer, read_result)))
      return false;
  } while (read_result > 0);
  return stream.Flush();
}

}  // namespace

CrashReportExceptionHandler::CrashReportExceptionHandler(
    CrashReportDatabase* database,
    CrashReportUploadThread* upload_thread,
    const std::map<std::string, std::string>* process_annotations,
    const std::vector<base::FilePath>* attachments,
    bool write_minidump_to_database,
    bool write_minidump_to_log,
    const UserStreamDataSources* user_stream_data_sources)
    : database_(database),
      upload_thread_(upload_thread),
      process_annotations_(process_annotations),
      attachments_(attachments),
      write_minidump_to_database_(write_minidump_to_database),
      write_minidump_to_log_(write_minidump_to_log),
      user_stream_data_sources_(user_stream_data_sources) {
  DCHECK(write_minidump_to_database_ | write_minidump_to_log_);
}

CrashReportExceptionHandler::~CrashReportExceptionHandler() = default;

bool CrashReportExceptionHandler::HandleException(
    pid_t client_process_id,
    uid_t client_uid,
    const ExceptionHandlerProtocol::ClientInformation& info,
    VMAddress requesting_thread_stack_address,
    pid_t* requesting_thread_id,
    UUID* local_report_id) {
  Metrics::ExceptionEncountered();

  {
    auto it = process_annotations_->find("run-uuid");
    if (it != process_annotations_->cend()) {
      namespace cld = backtrace::crash_loop_detection;
      UUID uuid;
      uuid.InitializeFromString(it->second);
      auto success = cld::CrashLoopDetectionSetCrashed(database_->DatabasePath(), uuid);
      if (!success)
        LOG(ERROR) << "Failed to set crash loop detection data for '" << it->second
          << "'";
    }
  }

  DirectPtraceConnection connection;
  if (!connection.Initialize(client_process_id)) {
    Metrics::ExceptionCaptureResult(
        Metrics::CaptureResult::kDirectPtraceFailed);
    return false;
  }

  return HandleExceptionWithConnection(&connection,
                                       info,
                                       client_uid,
                                       requesting_thread_stack_address,
                                       requesting_thread_id,
                                       local_report_id);
}

bool CrashReportExceptionHandler::HandleExceptionWithBroker(
    pid_t client_process_id,
    uid_t client_uid,
    const ExceptionHandlerProtocol::ClientInformation& info,
    int broker_sock,
    UUID* local_report_id) {
  Metrics::ExceptionEncountered();

  PtraceClient client;
  if (!client.Initialize(broker_sock, client_process_id)) {
    Metrics::ExceptionCaptureResult(
        Metrics::CaptureResult::kBrokeredPtraceFailed);
    return false;
  }

  return HandleExceptionWithConnection(
      &client, info, client_uid, 0, nullptr, local_report_id);
}

bool CrashReportExceptionHandler::HandleExceptionWithConnection(
    PtraceConnection* connection,
    const ExceptionHandlerProtocol::ClientInformation& info,
    uid_t client_uid,
    VMAddress requesting_thread_stack_address,
    pid_t* requesting_thread_id,
    UUID* local_report_id) {
  std::unique_ptr<ProcessSnapshotLinux> process_snapshot;
  std::unique_ptr<ProcessSnapshotSanitized> sanitized_snapshot;
  if (!CaptureSnapshot(connection,
                       info,
                       *process_annotations_,
                       client_uid,
                       requesting_thread_stack_address,
                       requesting_thread_id,
                       &process_snapshot,
                       &sanitized_snapshot)) {
    return false;
  }

  UUID client_id;
  Settings* const settings = database_->GetSettings();
  if (settings && settings->GetClientID(&client_id)) {
    process_snapshot->SetClientID(client_id);
  }

  return write_minidump_to_database_
             ? WriteMinidumpToDatabase(process_snapshot.get(),
                                       sanitized_snapshot.get(),
                                       write_minidump_to_log_,
                                       local_report_id)
             : WriteMinidumpToLog(process_snapshot.get(),
                                  sanitized_snapshot.get());
}

bool CrashReportExceptionHandler::WriteMinidumpToDatabase(
    ProcessSnapshotLinux* process_snapshot,
    ProcessSnapshotSanitized* sanitized_snapshot,
    bool write_minidump_to_log,
    UUID* local_report_id) {
  std::unique_ptr<CrashReportDatabase::NewReport> new_report;
  CrashReportDatabase::OperationStatus database_status =
      database_->PrepareNewCrashReport(&new_report);
  if (database_status != CrashReportDatabase::kNoError) {
    LOG(ERROR) << "PrepareNewCrashReport failed";
    Metrics::ExceptionCaptureResult(
        Metrics::CaptureResult::kPrepareNewCrashReportFailed);
    return false;
  }

  process_snapshot->SetReportID(new_report->ReportID());

  ProcessSnapshot* snapshot =
      sanitized_snapshot ? implicit_cast<ProcessSnapshot*>(sanitized_snapshot)
                         : implicit_cast<ProcessSnapshot*>(process_snapshot);

  MinidumpFileWriter minidump;
  minidump.InitializeFromSnapshot(snapshot);
  AddUserExtensionStreams(user_stream_data_sources_, snapshot, &minidump);

  if (!minidump.WriteEverything(new_report->Writer())) {
    LOG(ERROR) << "WriteEverything failed";
    Metrics::ExceptionCaptureResult(
        Metrics::CaptureResult::kMinidumpWriteFailed);
    return false;
  }

  bool write_minidump_to_log_succeed = false;
  if (write_minidump_to_log) {
    if (auto* file_reader = new_report->Reader()) {
      if (WriteMinidumpLogFromFile(file_reader))
        write_minidump_to_log_succeed = true;
      else
        LOG(ERROR) << "WriteMinidumpLogFromFile failed";
    }
  }

  for (const auto& attachment : (*attachments_)) {
    FileReader file_reader;
    if (!file_reader.Open(attachment)) {
      LOG(ERROR) << "attachment " << attachment.value().c_str()
                 << " couldn't be opened, skipping";
      continue;
    }

    base::FilePath filename = attachment.BaseName();
    FileWriter* file_writer = new_report->AddAttachment(filename.value());
    if (file_writer == nullptr) {
      LOG(ERROR) << "attachment " << filename.value().c_str()
                 << " couldn't be created, skipping";
      continue;
    }

    CopyFileContent(&file_reader, file_writer);
  }

  UUID uuid;
  database_status =
      database_->FinishedWritingCrashReport(std::move(new_report), &uuid);
  if (database_status != CrashReportDatabase::kNoError) {
    LOG(ERROR) << "FinishedWritingCrashReport failed";
    Metrics::ExceptionCaptureResult(
        Metrics::CaptureResult::kFinishedWritingCrashReportFailed);
    return false;
  }

  if (upload_thread_) {
    upload_thread_->ReportPending(uuid);
  }

  if (local_report_id != nullptr) {
    *local_report_id = uuid;
  }

  Metrics::ExceptionCaptureResult(Metrics::CaptureResult::kSuccess);

  // === 崩溃通知开始 ===
  {
    // 获取 handler 所在目录（与主程序同目录）作为 reports 路径的基础
    char exe_buf[4096] = {0};
    ssize_t len = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    std::string report_path;
    if (len > 0) {
      std::string full_path(exe_buf, static_cast<size_t>(len));
      size_t last_slash = full_path.find_last_of('/');
      if (last_slash != std::string::npos)
        report_path = full_path.substr(0, last_slash) + "/reports";
    }
    if (report_path.empty())
      report_path = "./reports";

    // 消息内容（转义单引号以防 shell 注入）
    auto escape_sq = [](std::string s) -> std::string {
      size_t pos = 0;
      while ((pos = s.find('\'', pos)) != std::string::npos) {
        s.replace(pos, 1, "'\\''");
        pos += 4;
      }
      return s;
    };

    std::string safe_path = escape_sq(report_path);
    std::string message =
        "程序发生了崩溃，已生成 dmp 文件。\\n\\n"
        "文件路径: " + report_path + "\\n\\n"
        "请将该目录下的所有文件发送给技术支持团队以便进行问题分析。";
    std::string safe_msg = escape_sq(message);

    // 复制路径到剪贴板（尝试 xclip / xsel）
    system(("echo -n '" + safe_path +
            "' | xclip -selection clipboard 2>/dev/null || "
            "echo -n '" + safe_path +
            "' | xsel --clipboard --input 2>/dev/null")
               .c_str());

    // 显示崩溃通知：依次尝试 zenity → kdialog → xmessage → stderr
    int ret = system(
        ("zenity --error --title='崩溃通知' --no-wrap --text='" +
         safe_msg + "' 2>/dev/null")
            .c_str());
    if (ret != 0)
      ret = system(
          ("kdialog --error '" + safe_msg +
           "' --title '崩溃通知' 2>/dev/null")
              .c_str());
    if (ret != 0)
      system(("xmessage -center '" + safe_msg + "' 2>/dev/null").c_str());
    if (ret != 0)
      fprintf(stderr,
              "[CrashReport] dmp saved to: %s\n", report_path.c_str());
  }
  // === 崩溃通知结束 ===

  return write_minidump_to_log ? write_minidump_to_log_succeed : true;
}

bool CrashReportExceptionHandler::WriteMinidumpToLog(
    ProcessSnapshotLinux* process_snapshot,
    ProcessSnapshotSanitized* sanitized_snapshot) {
  ProcessSnapshot* snapshot =
      sanitized_snapshot ? implicit_cast<ProcessSnapshot*>(sanitized_snapshot)
                         : implicit_cast<ProcessSnapshot*>(process_snapshot);
  MinidumpFileWriter minidump;
  minidump.InitializeFromSnapshot(snapshot);
  AddUserExtensionStreams(user_stream_data_sources_, snapshot, &minidump);

  OutputStreamFileWriter writer(std::make_unique<ZlibOutputStream>(
      ZlibOutputStream::Mode::kCompress,
      std::make_unique<Base94OutputStream>(
          Base94OutputStream::Mode::kEncode,
          std::make_unique<LogOutputStream>(std::make_unique<Logger>()))));
  if (!minidump.WriteMinidump(&writer, false /* allow_seek */)) {
    LOG(ERROR) << "WriteMinidump failed";
    return false;
  }
  return writer.Flush();
}

}  // namespace crashpad
