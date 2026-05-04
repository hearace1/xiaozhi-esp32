#include "scheduled_task_manager.h"

#include <cJSON.h>
#include <esp_log.h>
#include <time.h>

#include <algorithm>
#include <cstdio>

#include "application.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "settings.h"

#define TAG "Scheduler"

namespace {

constexpr const char* kNvsNamespace = "schedule";
constexpr const char* kNvsKeyTasks = "tasks";
constexpr const char* kNvsKeyNextId = "next_id";

const char* TriggerToString(ScheduledTaskManager::TriggerType t) {
    switch (t) {
        case ScheduledTaskManager::TriggerType::Once:   return "once";
        case ScheduledTaskManager::TriggerType::Daily:  return "daily";
        case ScheduledTaskManager::TriggerType::Weekly: return "weekly";
    }
    return "once";
}

bool ParseTrigger(const std::string& s, ScheduledTaskManager::TriggerType& out) {
    if (s == "once")   { out = ScheduledTaskManager::TriggerType::Once;   return true; }
    if (s == "daily")  { out = ScheduledTaskManager::TriggerType::Daily;  return true; }
    if (s == "weekly") { out = ScheduledTaskManager::TriggerType::Weekly; return true; }
    return false;
}

const char* ActionToString(ScheduledTaskManager::ActionType a) {
    switch (a) {
        case ScheduledTaskManager::ActionType::Alarm:      return "alarm";
        case ScheduledTaskManager::ActionType::InvokeTool: return "tool";
        case ScheduledTaskManager::ActionType::AiPrompt:   return "prompt";
    }
    return "alarm";
}

bool ParseAction(const std::string& s, ScheduledTaskManager::ActionType& out) {
    if (s == "alarm")  { out = ScheduledTaskManager::ActionType::Alarm;      return true; }
    if (s == "tool")   { out = ScheduledTaskManager::ActionType::InvokeTool; return true; }
    if (s == "prompt") { out = ScheduledTaskManager::ActionType::AiPrompt;   return true; }
    return false;
}

cJSON* SerializeTask(const ScheduledTaskManager::Task& t) {
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", t.id.c_str());
    cJSON_AddStringToObject(obj, "title", t.title.c_str());
    cJSON_AddStringToObject(obj, "trigger", TriggerToString(t.trigger_type));
    cJSON_AddNumberToObject(obj, "once_epoch", static_cast<double>(t.once_epoch));
    cJSON_AddNumberToObject(obj, "minute_of_day", t.minute_of_day);
    cJSON_AddNumberToObject(obj, "weekday_mask", t.weekday_mask);
    cJSON_AddStringToObject(obj, "action", ActionToString(t.action_type));
    cJSON_AddStringToObject(obj, "payload", t.action_payload.c_str());
    cJSON_AddBoolToObject(obj, "enabled", t.enabled);
    return obj;
}

bool DeserializeTask(const cJSON* obj, ScheduledTaskManager::Task& out) {
    if (!cJSON_IsObject(obj)) return false;

    auto get_str = [&](const char* key, std::string& dst) -> bool {
        auto* item = cJSON_GetObjectItem(obj, key);
        if (!cJSON_IsString(item)) return false;
        dst = item->valuestring;
        return true;
    };

    std::string trigger_str, action_str;
    if (!get_str("id", out.id) || !get_str("trigger", trigger_str) ||
        !get_str("action", action_str)) {
        return false;
    }
    get_str("title", out.title);
    get_str("payload", out.action_payload);

    if (!ParseTrigger(trigger_str, out.trigger_type)) return false;
    if (!ParseAction(action_str, out.action_type)) return false;

    auto* once_epoch = cJSON_GetObjectItem(obj, "once_epoch");
    if (cJSON_IsNumber(once_epoch)) {
        out.once_epoch = static_cast<int64_t>(once_epoch->valuedouble);
    }
    auto* mod = cJSON_GetObjectItem(obj, "minute_of_day");
    if (cJSON_IsNumber(mod)) out.minute_of_day = static_cast<int16_t>(mod->valueint);
    auto* mask = cJSON_GetObjectItem(obj, "weekday_mask");
    if (cJSON_IsNumber(mask)) out.weekday_mask = static_cast<uint8_t>(mask->valueint);
    auto* enabled = cJSON_GetObjectItem(obj, "enabled");
    out.enabled = cJSON_IsBool(enabled) ? cJSON_IsTrue(enabled) : true;

    return true;
}

std::string FormatLocal(int64_t epoch) {
    if (epoch <= 0) return "";
    time_t t = static_cast<time_t>(epoch);
    struct tm tm_local;
    localtime_r(&t, &tm_local);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_local);
    return buf;
}

}  // namespace

ScheduledTaskManager& ScheduledTaskManager::GetInstance() {
    static ScheduledTaskManager instance;
    return instance;
}

void ScheduledTaskManager::Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return;
    initialized_ = true;
    LoadFromNvs();
    RecomputeAllNextFire(static_cast<int64_t>(time(nullptr)));
    ESP_LOGI(TAG, "Initialized with %d task(s)", static_cast<int>(tasks_.size()));
}

void ScheduledTaskManager::LoadFromNvs() {
    Settings settings(kNvsNamespace, false);
    next_id_ = static_cast<int>(settings.GetInt(kNvsKeyNextId, 1));
    std::string blob = settings.GetString(kNvsKeyTasks, "");
    if (blob.empty()) return;

    cJSON* root = cJSON_Parse(blob.c_str());
    if (!cJSON_IsArray(root)) {
        ESP_LOGW(TAG, "Invalid tasks blob in NVS, resetting");
        if (root) cJSON_Delete(root);
        return;
    }
    int count = cJSON_GetArraySize(root);
    for (int i = 0; i < count && tasks_.size() < kMaxTasks; ++i) {
        Task t;
        if (DeserializeTask(cJSON_GetArrayItem(root, i), t)) {
            tasks_.push_back(std::move(t));
        } else {
            ESP_LOGW(TAG, "Skipped malformed task at index %d", i);
        }
    }
    cJSON_Delete(root);
}

void ScheduledTaskManager::PersistLocked() const {
    cJSON* root = cJSON_CreateArray();
    for (const auto& t : tasks_) {
        cJSON_AddItemToArray(root, SerializeTask(t));
    }
    char* json_str = cJSON_PrintUnformatted(root);
    Settings settings(kNvsNamespace, true);
    settings.SetString(kNvsKeyTasks, json_str ? json_str : "[]");
    settings.SetInt(kNvsKeyNextId, next_id_);
    if (json_str) cJSON_free(json_str);
    cJSON_Delete(root);
}

std::string ScheduledTaskManager::AllocateIdLocked() {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "t%d", next_id_++);
    return buf;
}

int64_t ScheduledTaskManager::ComputeNextFireEpoch(const Task& task, int64_t now) const {
    if (!task.enabled) return 0;

    if (task.trigger_type == TriggerType::Once) {
        return task.once_epoch > now ? task.once_epoch : 0;
    }

    time_t t_now = static_cast<time_t>(now);
    struct tm tm_now;
    localtime_r(&t_now, &tm_now);

    auto compute_for_date = [&](int day_offset) -> int64_t {
        struct tm tm_target = tm_now;
        tm_target.tm_sec = 0;
        tm_target.tm_min = task.minute_of_day % 60;
        tm_target.tm_hour = task.minute_of_day / 60;
        tm_target.tm_mday += day_offset;
        tm_target.tm_isdst = -1;
        time_t candidate = mktime(&tm_target);
        return static_cast<int64_t>(candidate);
    };

    if (task.trigger_type == TriggerType::Daily) {
        int64_t today = compute_for_date(0);
        if (today > now) return today;
        return compute_for_date(1);
    }

    // Weekly
    if (task.weekday_mask == 0) return 0;
    for (int offset = 0; offset < 8; ++offset) {
        int64_t candidate = compute_for_date(offset);
        if (candidate <= now) continue;
        time_t c_t = static_cast<time_t>(candidate);
        struct tm tm_c;
        localtime_r(&c_t, &tm_c);
        if (task.weekday_mask & (1u << tm_c.tm_wday)) {
            return candidate;
        }
    }
    return 0;
}

void ScheduledTaskManager::RecomputeAllNextFire(int64_t now) {
    for (auto& t : tasks_) {
        t.next_fire_epoch = ComputeNextFireEpoch(t, now);
    }
    std::sort(tasks_.begin(), tasks_.end(), [](const Task& a, const Task& b) {
        int64_t av = a.next_fire_epoch > 0 ? a.next_fire_epoch : INT64_MAX;
        int64_t bv = b.next_fire_epoch > 0 ? b.next_fire_epoch : INT64_MAX;
        return av < bv;
    });
}

std::string ScheduledTaskManager::AddTask(Task task) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tasks_.size() >= kMaxTasks) {
        ESP_LOGW(TAG, "Task capacity (%u) reached", static_cast<unsigned>(kMaxTasks));
        return "";
    }
    task.id = AllocateIdLocked();
    task.enabled = true;
    int64_t now = static_cast<int64_t>(time(nullptr));
    task.next_fire_epoch = ComputeNextFireEpoch(task, now);
    ESP_LOGI(TAG, "Added task %s: trigger=%s once_epoch=%ld minute_of_day=%d "
                  "weekday_mask=0x%02x action=%s next_fire=%ld (in %ld s) now=%ld",
             task.id.c_str(), TriggerToString(task.trigger_type),
             (long)task.once_epoch, task.minute_of_day, task.weekday_mask,
             ActionToString(task.action_type),
             (long)task.next_fire_epoch,
             (long)(task.next_fire_epoch > now ? task.next_fire_epoch - now : -1),
             (long)now);
    std::string new_id = task.id;
    tasks_.push_back(std::move(task));
    RecomputeAllNextFire(now);
    PersistLocked();
    return new_id;
}

bool ScheduledTaskManager::CancelTask(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(tasks_.begin(), tasks_.end(),
                           [&](const Task& t) { return t.id == id; });
    if (it == tasks_.end()) return false;
    tasks_.erase(it);
    PersistLocked();
    ESP_LOGI(TAG, "Cancelled task %s", id.c_str());
    return true;
}

std::string ScheduledTaskManager::FetchDueReminder() {
    static constexpr int64_t kPromptTtlSeconds = 120;  // 太久没人取就丢弃

    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now = static_cast<int64_t>(time(nullptr));

    if (pending_prompt_text_.empty()) {
        ESP_LOGI(TAG, "FetchDueReminder: no pending prompt");
        return "{}";
    }
    if (now - pending_prompt_epoch_ > kPromptTtlSeconds) {
        ESP_LOGW(TAG, "FetchDueReminder: pending prompt expired (age %lds), discarding",
                 (long)(now - pending_prompt_epoch_));
        pending_prompt_text_.clear();
        pending_prompt_epoch_ = 0;
        return "{}";
    }

    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "text", pending_prompt_text_.c_str());
    cJSON_AddNumberToObject(obj, "age_s", static_cast<double>(now - pending_prompt_epoch_));
    char* s = cJSON_PrintUnformatted(obj);
    std::string result = s ? s : "{}";
    if (s) cJSON_free(s);
    cJSON_Delete(obj);

    ESP_LOGI(TAG, "FetchDueReminder: delivering '%s'", pending_prompt_text_.c_str());
    pending_prompt_text_.clear();
    pending_prompt_epoch_ = 0;
    return result;
}

std::string ScheduledTaskManager::ListTasksJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    cJSON* arr = cJSON_CreateArray();
    for (const auto& t : tasks_) {
        cJSON* obj = SerializeTask(t);
        cJSON_AddStringToObject(obj, "next_fire_local",
                                FormatLocal(t.next_fire_epoch).c_str());
        cJSON_AddItemToArray(arr, obj);
    }
    char* s = cJSON_PrintUnformatted(arr);
    std::string result = s ? s : "[]";
    if (s) cJSON_free(s);
    cJSON_Delete(arr);
    return result;
}

void ScheduledTaskManager::Tick() {
    // 本函数在 Application 主循环每秒调用一次，自身已在主任务上下文
    if (!initialized_) return;

    int64_t now = static_cast<int64_t>(time(nullptr));

    // Diagnostic: every 30s, print wall clock + next pending fire so we can
    // tell from the log whether time has been synced and whether tasks exist.
    static int64_t last_diag = 0;
    if (now - last_diag >= 30 || now < last_diag) {
        last_diag = now;
        int64_t next = INT64_MAX;
        std::string next_id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& t : tasks_) {
                if (t.enabled && t.next_fire_epoch > 0 && t.next_fire_epoch < next) {
                    next = t.next_fire_epoch;
                    next_id = t.id;
                }
            }
        }
        // Use %ld (long) instead of %lld — ESP-IDF's nano printf doesn't
        // support long long. time_t fits in 32 bits until 2038 anyway.
        if (next == INT64_MAX) {
            ESP_LOGI(TAG, "tick: now=%ld synced=%d tasks=%u no-pending",
                     (long)now, now >= 1700000000 ? 1 : 0,
                     (unsigned)tasks_.size());
        } else {
            ESP_LOGI(TAG, "tick: now=%ld synced=%d next=%s in %ld s",
                     (long)now, now >= 1700000000 ? 1 : 0,
                     next_id.c_str(), (long)(next - now));
        }
    }

    // 系统时间还没被 settimeofday 设置过时，now 会是很小的值（1970 附近）；跳过
    if (now < 1700000000) return;

    // ─── pending prompt 投递 ───
    // FireTaskLocked 只把 prompt 文本写到 pending 槽。这里负责挑设备空闲的瞬间
    // 把固定触发词 "【定时】" 发出去，避免在用户聊天时打断对话。
    {
        static constexpr const char* kPromptTrigger = "【定时】";
        static constexpr int64_t kPromptDeliveryTtl = 120;  // 太久没机会发就放弃

        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_prompt_text_.empty()) {
            int64_t age = now - pending_prompt_epoch_;
            if (age > kPromptDeliveryTtl) {
                ESP_LOGW(TAG, "Pending prompt expired (age %lds, sent=%d)",
                         (long)age, pending_wake_sent_ ? 1 : 0);
                pending_prompt_text_.clear();
                pending_prompt_epoch_ = 0;
                pending_wake_sent_ = false;
            } else if (!pending_wake_sent_ &&
                       Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                ESP_LOGI(TAG, "Delivering pending prompt now (idle, age %lds)", (long)age);
                pending_wake_sent_ = true;
                Application::GetInstance().Schedule([]() {
                    Application::GetInstance().WakeWordInvoke(kPromptTrigger);
                });
            }
            // 非空闲且未发：静等下一次 Tick
            // 已发但未 fetch：等 AI 调 fetch_due_reminder 或 TTL 到期
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<size_t> to_fire;
    for (size_t i = 0; i < tasks_.size(); ++i) {
        const auto& t = tasks_[i];
        if (!t.enabled || t.next_fire_epoch == 0) continue;
        if (t.next_fire_epoch <= now) to_fire.push_back(i);
    }
    if (to_fire.empty()) return;

    // 从后向前处理，Once 任务直接从 vector 移除不影响前面的索引
    std::sort(to_fire.begin(), to_fire.end(), std::greater<size_t>());
    for (size_t idx : to_fire) {
        Task task_copy = tasks_[idx];  // 复制出来再执行，避免回调重入时引用失效
        if (task_copy.trigger_type == TriggerType::Once) {
            tasks_.erase(tasks_.begin() + idx);
        }
        try {
            FireTaskLocked(task_copy, now);
        } catch (const std::exception& e) {
            ESP_LOGW(TAG, "Task %s fire failed: %s", task_copy.id.c_str(), e.what());
        }
    }

    RecomputeAllNextFire(now);
    PersistLocked();
}

void ScheduledTaskManager::FireTaskLocked(Task& task, int64_t now) {
    ESP_LOGI(TAG, "Firing task %s (action=%s)",
             task.id.c_str(), ActionToString(task.action_type));

    // 但若还是周期性任务，重算下次并写回原任务（如果还在列表里）
    if (task.trigger_type != TriggerType::Once) {
        int64_t next = ComputeNextFireEpoch(task, now + 1);
        for (auto& existing : tasks_) {
            if (existing.id == task.id) {
                existing.next_fire_epoch = next;
                break;
            }
        }
    }

    switch (task.action_type) {
        case ActionType::Alarm: {
            std::string message;
            // Use OGG_SUCCESS — same sound that plays at activation, so we know
            // it's audible. OGG_EXCLAMATION sometimes fails to play after long idle
            // because the codec output may have powered down.
            std::string_view sound = Lang::Sounds::OGG_SUCCESS;
            if (!task.action_payload.empty()) {
                cJSON* payload = cJSON_Parse(task.action_payload.c_str());
                if (cJSON_IsObject(payload)) {
                    auto* msg = cJSON_GetObjectItem(payload, "message");
                    if (cJSON_IsString(msg)) message = msg->valuestring;
                }
                if (payload) cJSON_Delete(payload);
            }
            if (message.empty()) message = task.title;
            if (message.empty()) message = "Reminder";
            ESP_LOGI(TAG, "Alarm: title='%s' message='%s' sound_size=%u",
                     task.title.c_str(), message.c_str(), (unsigned)sound.size());
            Application::GetInstance().Alert(
                task.title.empty() ? "Reminder" : task.title.c_str(),
                message.c_str(),
                "thinking",
                sound);
            // Belt-and-suspenders: play sound twice with a small delay so even if the
            // first round happened while codec was waking up, the second hits.
            Application::GetInstance().Schedule([sound]() {
                Application::GetInstance().GetAudioService().PlaySound(sound);
            });
            break;
        }
        case ActionType::InvokeTool: {
            if (task.action_payload.empty()) {
                ESP_LOGW(TAG, "InvokeTool task has empty payload");
                break;
            }
            cJSON* payload = cJSON_Parse(task.action_payload.c_str());
            if (!cJSON_IsObject(payload)) {
                ESP_LOGW(TAG, "InvokeTool payload is not a JSON object");
                if (payload) cJSON_Delete(payload);
                break;
            }
            auto* name = cJSON_GetObjectItem(payload, "name");
            auto* args = cJSON_GetObjectItem(payload, "args");
            if (!cJSON_IsString(name)) {
                ESP_LOGW(TAG, "InvokeTool payload missing 'name'");
                cJSON_Delete(payload);
                break;
            }
            try {
                std::string result = McpServer::GetInstance().InvokeTool(
                    name->valuestring, args);
                ESP_LOGI(TAG, "Tool %s invoked: %s",
                         name->valuestring,
                         result.size() > 128 ? "<truncated>" : result.c_str());
            } catch (const std::exception& e) {
                ESP_LOGW(TAG, "InvokeTool %s failed: %s", name->valuestring, e.what());
            }
            cJSON_Delete(payload);
            break;
        }
        case ActionType::AiPrompt: {
            // 两段式 prompt 流（详见 Tick 里的发送逻辑）：
            //   ① FireTask 仅把内容写到 pending 槽，不立刻动 wake_word
            //   ② Tick 在设备空闲时才发触发词 "【定时】"（避免打断当前对话）
            //   ③ 服务端 AI 收到触发词后调 self.schedule.fetch_due_reminder 取走内容
            std::string text;
            if (!task.action_payload.empty()) {
                cJSON* payload = cJSON_Parse(task.action_payload.c_str());
                if (cJSON_IsObject(payload)) {
                    auto* t = cJSON_GetObjectItem(payload, "text");
                    if (cJSON_IsString(t)) text = t->valuestring;
                }
                if (payload) cJSON_Delete(payload);
            }
            if (text.empty()) text = task.title;
            if (text.empty()) {
                ESP_LOGW(TAG, "AiPrompt task has no text or title");
                break;
            }

            // 占用 pending 槽（已持有 mutex_）。下一个 Tick 看到设备空闲会自动送出。
            pending_prompt_text_ = text;
            pending_prompt_epoch_ = now;
            pending_wake_sent_ = false;
            ESP_LOGI(TAG, "Pending prompt set: '%s' (waiting for idle to deliver)",
                     text.c_str());
            break;
        }
    }
}
