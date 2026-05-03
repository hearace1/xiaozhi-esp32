#ifndef SCHEDULED_TASK_MANAGER_H
#define SCHEDULED_TASK_MANAGER_H

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class ScheduledTaskManager {
public:
    enum class TriggerType {
        Once,
        Daily,
        Weekly,
    };

    enum class ActionType {
        Alarm,       // 播放提示音 + 屏幕通知 + 自定义提醒文字
        InvokeTool,  // 调用其它已注册的 MCP 工具
        AiPrompt,    // 向 AI 注入一条提示，触发主动对话
    };

    struct Task {
        std::string id;
        std::string title;
        TriggerType trigger_type = TriggerType::Once;
        int64_t once_epoch = 0;          // Unix 秒，仅 Once 使用
        int16_t minute_of_day = 0;       // 0..1439，Daily/Weekly 使用（本地时区）
        uint8_t weekday_mask = 0;        // bit0=周日 .. bit6=周六，Weekly 使用
        ActionType action_type = ActionType::Alarm;
        std::string action_payload;      // JSON 字符串，按 action_type 解释
        bool enabled = true;
        int64_t next_fire_epoch = 0;     // 运行时计算，不持久化
    };

    // 单个任务 JSON 序列化后不超过 ~512B；硬上限 32 条
    static constexpr size_t kMaxTasks = 32;

    static ScheduledTaskManager& GetInstance();

    // 仅允许调用一次；在主任务中调用
    void Initialize();

    // 返回新建任务 id；容量满返回空字符串
    // task.id 会被内部覆盖；caller 填 title / trigger / action 相关字段即可
    std::string AddTask(Task task);

    // 按 id 取消；返回 true 表示存在且被删除
    bool CancelTask(const std::string& id);

    // 返回当前任务列表的 JSON 数组字符串（给 MCP list 工具用）
    std::string ListTasksJson() const;

    // 每秒由 Application 主循环调用一次
    void Tick();

    // AiPrompt 流：FireTask 把待提醒文本入队 + 发短 wake_word，
    // 服务端 AI 收到 wake_word 后调 fetch_due_reminder 取走真正内容。
    // 调用一次即清空当前 slot；超过 TTL 的也会被自动丢弃。
    std::string FetchDueReminder();

private:
    ScheduledTaskManager() = default;
    ScheduledTaskManager(const ScheduledTaskManager&) = delete;
    ScheduledTaskManager& operator=(const ScheduledTaskManager&) = delete;

    void LoadFromNvs();
    void PersistLocked() const;
    void FireTaskLocked(Task& task, int64_t now);
    int64_t ComputeNextFireEpoch(const Task& task, int64_t now) const;
    void RecomputeAllNextFire(int64_t now);
    std::string AllocateIdLocked();

    mutable std::mutex mutex_;
    std::vector<Task> tasks_;
    int next_id_ = 1;
    bool initialized_ = false;

    // AiPrompt 待取队列（最多 1 条，新值覆盖旧值，过期自动失效）
    std::string pending_prompt_text_;
    int64_t pending_prompt_epoch_ = 0;
};

#endif // SCHEDULED_TASK_MANAGER_H
