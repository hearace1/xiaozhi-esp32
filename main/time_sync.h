#ifndef TIME_SYNC_H
#define TIME_SYNC_H

// 启动 SNTP 周期性校时。幂等：多次调用仅生效一次。
// 注意：ota.cc 将服务器时间与时区偏移合并后写入系统时钟（「本地时间伪装成 UTC」），
//      因此本模块在 SNTP 同步回调里会再次把 true-UTC 加上 tz_offset 以维持同一语义。
// 需要网络就绪后再调用；在 Application::HandleNetworkConnectedEvent() 中调用是合适位置。
void InitializeSntp();

// 保存当前 tz 偏移（单位：分钟）到 NVS，供 SNTP 同步回调读取。
// 由 ota.cc 收到 server_time.timezone_offset 时调用。
void SaveTimezoneOffsetMinutes(int minutes);

#endif  // TIME_SYNC_H
