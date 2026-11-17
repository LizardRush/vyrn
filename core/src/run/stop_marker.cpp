#include "run/stop_marker.h"

#include "platform/files.h"


#include <cerrno>
#include <chrono>
#include <filesystem>

namespace vyrn::run {

namespace {

namespace fs = std::filesystem;

std::string marker_path(const std::string& dir) { return (fs::path(dir) / kStopMarkerName).string(); }

}  // namespace

bool take_stop_marker(const std::string& dir) {
  const std::string path = marker_path(dir);
  platform::FileInfo st;
  if (platform::file_info(path, false, st) != 0) return false;
  std::error_code ec;
  fs::remove(path, ec);   // a symlink is removed itself, never what it points to
  return true;
}

StopMarkerWatch::StopMarkerWatch(RunContext& ctx, std::string project_dir, double every_s)
    : ctx_(ctx), dir_(std::move(project_dir)), every_s_(every_s > 0 ? every_s : 0.5) {
  const std::string path = marker_path(dir_);
  platform::FileInfo st;
  if (platform::file_info(path, false, st) == 0) {   // left over from before this run: not meant for it
    std::error_code ec;
    fs::remove(path, ec);
    platform::FileInfo again;
    if (platform::file_info(path, false, again) == 0 && again.device == st.device && again.inode == st.inode) {
      stale_kept_ = true;   // e.g. a read-only folder: ignore this very file, but not a new one
      stale_dev_ = st.device;
      stale_ino_ = st.inode;
    }
  }
  thread_ = std::thread([this] { poll(); });
}

StopMarkerWatch::~StopMarkerWatch() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    done_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

bool StopMarkerWatch::is_stale_leftover() const {
  if (!stale_kept_) return false;
  platform::FileInfo st;
  return platform::file_info(marker_path(dir_), false, st) == 0 && st.device == stale_dev_ && st.inode == stale_ino_;
}

void StopMarkerWatch::poll() {
  bool asked = false;
  std::unique_lock<std::mutex> lock(mu_);
  for (;;) {
    cv_.wait_for(lock, std::chrono::duration<double>(every_s_), [this] { return done_; });
    if (done_) return;
    lock.unlock();
    if (!is_stale_leftover() && take_stop_marker(dir_) && !asked) {
      asked = true;
      const bool training = ctx_.config().action == RunConfig::Action::Train;
      ctx_.app_log(std::string("Another program asked this run to stop (it made the file ") + kStopMarkerName +
                   "): stopping" + (training ? " and saving the brain." : "."));
      ctx_.request_stop();
    }
    lock.lock();
  }
}

}  // namespace vyrn::run
