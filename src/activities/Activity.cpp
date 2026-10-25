#include "Activity.h"

#include "ActivityManager.h"

void Activity::onEnter() { LOG_DBG("ACT", "Entering activity: %s", name.c_str()); }

void Activity::onExit() { LOG_DBG("ACT", "Exiting activity: %s", name.c_str()); }

void Activity::requestUpdate(bool immediate) { activityManager.requestUpdate(immediate); }

void Activity::requestUpdateAndWait() { activityManager.requestUpdateAndWait(); }

void Activity::onGoHome(HomeMenuItem item) { activityManager.goHome(item); }

void Activity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void Activity::startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler handler) {
  this->resultHandler = std::move(handler);
  activityManager.pushActivity(std::move(activity));
}

void Activity::setResult(ActivityResult&& newResult) {
  this->result = std::move(newResult);
  markActivityResultDelivered(this->result);
}

void Activity::finish() { activityManager.popActivity(); }
