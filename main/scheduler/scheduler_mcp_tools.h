#ifndef SCHEDULER_MCP_TOOLS_H
#define SCHEDULER_MCP_TOOLS_H

class McpServer;

// 注册 self.schedule.add_task / list_tasks / cancel_task 三个工具。
// 在 McpServer::AddCommonTools() 之后调用，保证 ScheduledTaskManager 已 Initialize。
void RegisterSchedulerTools(McpServer& server);

#endif  // SCHEDULER_MCP_TOOLS_H
