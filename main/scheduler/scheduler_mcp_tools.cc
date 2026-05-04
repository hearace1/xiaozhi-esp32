#include "scheduler_mcp_tools.h"

#include <cJSON.h>
#include <esp_log.h>
#include <time.h>

#include <cstdio>
#include <cstring>

#include "mcp_server.h"
#include "scheduled_task_manager.h"

#define TAG "SchedulerTools"

namespace {

using Manager = ScheduledTaskManager;

// 解析 "HH:MM" 为分钟数（0..1439），失败返回 -1
int ParseHHMM(const std::string& s) {
    if (s.size() < 3) return -1;
    int h = 0, m = 0;
    if (std::sscanf(s.c_str(), "%d:%d", &h, &m) != 2) return -1;
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

// 解析 ISO 8601 格式 "YYYY-MM-DD HH:MM:SS" 或 "YYYY-MM-DDTHH:MM:SS"
// 返回 Unix 秒（按当前时区理解，兼容 ota.cc 的「本地时间伪装成 UTC」方案）
int64_t ParseLocalDateTime(const std::string& s) {
    struct tm tm_v{};
    int y, mo, d, h, mi, se;
    int n = std::sscanf(s.c_str(), "%d-%d-%d%*c%d:%d:%d",
                        &y, &mo, &d, &h, &mi, &se);
    if (n != 6) return 0;
    tm_v.tm_year = y - 1900;
    tm_v.tm_mon = mo - 1;
    tm_v.tm_mday = d;
    tm_v.tm_hour = h;
    tm_v.tm_min = mi;
    tm_v.tm_sec = se;
    tm_v.tm_isdst = -1;
    time_t epoch = mktime(&tm_v);
    return static_cast<int64_t>(epoch);
}

// "0,1,2" → bitmask, 0=Sunday..6=Saturday
uint8_t ParseWeekdays(const std::string& s) {
    uint8_t mask = 0;
    const char* p = s.c_str();
    while (*p) {
        if (*p >= '0' && *p <= '6') {
            mask |= (1u << (*p - '0'));
        }
        ++p;
    }
    return mask;
}

}  // namespace

void RegisterSchedulerTools(McpServer& server) {
    server.AddTool(
        "self.schedule.add_task",
        "Create a scheduled task that fires at a future time. Choose the action type carefully based on user intent.\n"
        "Arguments:\n"
        "  title: short human-readable label (e.g. '起床闹钟', '吃药提醒'). For action='alarm' this also becomes the popup message if payload is empty.\n"
        "  trigger: one of 'once' | 'daily' | 'weekly'\n"
        "  time: for trigger='once' use 'YYYY-MM-DD HH:MM:SS' in local time; for 'daily'/'weekly' use 'HH:MM'\n"
        "  weekdays: only for trigger='weekly'; comma-separated digits 0..6 (0=Sunday). For Mon–Fri use '1,2,3,4,5'. Pass empty string otherwise.\n"
        "  action: one of 'alarm' | 'tool' | 'prompt'\n"
        "    alarm  → SAFE DEFAULT for any reminder. Plays sound + shows popup with a message on screen. Payload optional; format {\"message\":\"...\"}; empty payload uses title.\n"
        "    tool   → use ONLY when the user wants the device to perform a concrete tool action at T (e.g. set volume, turn on a light). Payload REQUIRED: {\"name\":\"<tool name>\",\"args\":{...}}.\n"
        "    prompt → use when user wants the AI to PROACTIVELY SPEAK at T. The text can be ANY length (held on device, fetched through self.schedule.fetch_due_reminder). When the prompt fires the device sends a fixed trigger '【定时】' to you, and you MUST call fetch_due_reminder to retrieve this exact text and deliver it. Payload REQUIRED: {\"text\":\"<the reminder content as you'd say it to the user>\"}. Examples: text='莹仔该去写作业啦', text='喝水时间到了，莹仔记得多喝点温水哦', text='起床啦小懒虫～'.\n"
        "  payload: JSON string as described above. For 'tool' and 'prompt' it is REQUIRED.\n"
        "Examples:\n"
        "  User says '5 分钟后提醒我'           → action='alarm',  title='提醒', payload=''\n"
        "  User says '提醒我 3 点开会'          → action='alarm',  title='开会', payload='{\"message\":\"开会时间到了\"}'\n"
        "  User says '每天 22 点把音量调到 20'  → action='tool',   payload='{\"name\":\"self.audio_speaker.set_volume\",\"args\":{\"volume\":20}}'\n"
        "  User says '5 分钟后让小智叫我洗漱'   → action='prompt', payload='{\"text\":\"该去洗漱啦\"}'  (short, AI naturally reminds)\n"
        "Returns: the new task id, or a string starting with 'ERROR:' if input is invalid.",
        PropertyList({
            Property("title", kPropertyTypeString),
            Property("trigger", kPropertyTypeString),
            Property("time", kPropertyTypeString),
            Property("weekdays", kPropertyTypeString, std::string("")),
            Property("action", kPropertyTypeString),
            Property("payload", kPropertyTypeString, std::string("")),
        }),
        [](const PropertyList& props) -> ReturnValue {
            Manager::Task task;
            task.title = props["title"].value<std::string>();

            std::string trigger = props["trigger"].value<std::string>();
            if (trigger == "once")   task.trigger_type = Manager::TriggerType::Once;
            else if (trigger == "daily") task.trigger_type = Manager::TriggerType::Daily;
            else if (trigger == "weekly") task.trigger_type = Manager::TriggerType::Weekly;
            else return std::string("ERROR: invalid trigger, expected once|daily|weekly");

            std::string time_str = props["time"].value<std::string>();
            if (task.trigger_type == Manager::TriggerType::Once) {
                task.once_epoch = ParseLocalDateTime(time_str);
                if (task.once_epoch <= 0)
                    return std::string("ERROR: could not parse time as 'YYYY-MM-DD HH:MM:SS'");
                int64_t now = static_cast<int64_t>(time(nullptr));
                if (task.once_epoch <= now)
                    return std::string("ERROR: 'time' is in the past");
            } else {
                int mod = ParseHHMM(time_str);
                if (mod < 0) return std::string("ERROR: could not parse time as 'HH:MM'");
                task.minute_of_day = static_cast<int16_t>(mod);
            }

            if (task.trigger_type == Manager::TriggerType::Weekly) {
                task.weekday_mask = ParseWeekdays(props["weekdays"].value<std::string>());
                if (task.weekday_mask == 0)
                    return std::string("ERROR: weekly trigger requires non-empty weekdays like '1,2,3,4,5'");
            }

            std::string action = props["action"].value<std::string>();
            if (action == "alarm")       task.action_type = Manager::ActionType::Alarm;
            else if (action == "tool")   task.action_type = Manager::ActionType::InvokeTool;
            else if (action == "prompt") task.action_type = Manager::ActionType::AiPrompt;
            else return std::string("ERROR: invalid action, expected alarm|tool|prompt");

            task.action_payload = props["payload"].value<std::string>();

            // Validate payload according to action type so AI gets fast feedback.
            // Empty alarm payload is fine — title will be used at fire time.
            if (task.action_type != Manager::ActionType::Alarm || !task.action_payload.empty()) {
                cJSON* payload = task.action_payload.empty()
                                     ? nullptr
                                     : cJSON_Parse(task.action_payload.c_str());
                if (!task.action_payload.empty() && !cJSON_IsObject(payload)) {
                    if (payload) cJSON_Delete(payload);
                    return std::string("ERROR: payload must be a valid JSON object string");
                }
                if (task.action_type == Manager::ActionType::InvokeTool) {
                    auto* name = payload ? cJSON_GetObjectItem(payload, "name") : nullptr;
                    if (!cJSON_IsString(name) || std::string(name->valuestring).empty()) {
                        if (payload) cJSON_Delete(payload);
                        return std::string("ERROR: action='tool' requires payload {\"name\":\"<tool name>\",\"args\":{...}}");
                    }
                }
                if (task.action_type == Manager::ActionType::AiPrompt) {
                    auto* text = payload ? cJSON_GetObjectItem(payload, "text") : nullptr;
                    if (!cJSON_IsString(text) || std::string(text->valuestring).empty()) {
                        if (payload) cJSON_Delete(payload);
                        return std::string("ERROR: action='prompt' requires payload {\"text\":\"<reminder content>\"}");
                    }
                    // No tight length limit any more — content is fetched via MCP, not the wake-word channel.
                    if (std::string(text->valuestring).size() > 256) {
                        if (payload) cJSON_Delete(payload);
                        return std::string("ERROR: prompt text exceeds 256 bytes; please shorten");
                    }
                }
                if (payload) cJSON_Delete(payload);
            }

            std::string id = Manager::GetInstance().AddTask(std::move(task));
            if (id.empty())
                return std::string("ERROR: schedule is full (max 32 tasks), please cancel an existing one first");
            return id;
        });

    server.AddTool(
        "self.schedule.list_tasks",
        "List all scheduled tasks. Returns a JSON array; each item has id, title, trigger, action, payload, "
        "next_fire_local (human-readable local time of next firing) and other fields.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            return Manager::GetInstance().ListTasksJson();
        });

    server.AddTool(
        "self.schedule.cancel_task",
        "Cancel a scheduled task by its id (as returned from list_tasks / add_task). "
        "Returns true if the task was found and removed, false otherwise.",
        PropertyList({
            Property("id", kPropertyTypeString),
        }),
        [](const PropertyList& props) -> ReturnValue {
            return Manager::GetInstance().CancelTask(props["id"].value<std::string>());
        });

    server.AddTool(
        "self.schedule.fetch_due_reminder",
        "Fetch the content of a scheduled prompt that just fired on the device.\n"
        "**CRITICAL: Always call this tool when ANY of the conditions below match — DO NOT answer from memory or context.** "
        "The pending reminder is stored on the device's hardware NVS, NOT in your conversation context. Even if you set the task earlier in this conversation, the device may have updated, expired, or replaced the pending text since then; only this tool returns the current truth.\n"
        "Call this tool whenever:\n"
        "  (a) The user's input contains or equals the trigger phrase '【定时】'. The device sends this automatically when an action='prompt' task fires while the device is idle.\n"
        "  (b) The user asks about a reminder in any phrasing — examples (NOT exhaustive): '什么提醒', '有什么提醒', '有提醒吗', '刚才什么提醒', '刚刚提醒了什么', '提醒我什么', '查一下提醒', '刚才弹了什么', '刚才屏幕上是什么', '到点了吗', '现在该做什么了', or anything similar in any language. When in doubt, CALL THE TOOL.\n"
        "Returns: a JSON object {\"text\":\"<the reminder content>\",\"age_s\":<seconds since fired>} when there is a pending reminder, or an empty object {} when none.\n"
        "After getting the text, deliver it to the user as a natural reminder (e.g. '到时间啦，<text>～'). If the result is empty, just say there's no pending reminder right now — do not invent one or recall an earlier one from memory.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            return Manager::GetInstance().FetchDueReminder();
        });

    ESP_LOGI(TAG, "Registered 4 scheduler tools");
}
