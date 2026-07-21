# 南桥全异步统一迁移计划

目标：把 southbridge 从「专用线程 + 同步 hiredis/libpq + `sleep_for`/`future.get`」统一到
「ruvia worker io_context 上的协程 + `co_await` async Redis/DB」，落实「全异步、禁止同步阻塞」约束。

> 破坏性更新只涉及 ruvia model 层，底层 Redis/DB/io_context API 稳定，可照当前 ruvia 写。
> 本迁移正确性是**运行时时序/存活性/不丢消息**，构建绿不代表正确，每个里程碑需在
> 能跑 Redis + Postgres + 设备回环的环境里验证。

## 稳定的 ruvia 目标 API（已核实）

- `WebWorkerHandle::post(Fn)` / `postTask(MoveOnlyFunction<Task<void>(WebWorkerContext&)>)`：
  把协程投递到某 worker 的 io_context 运行。`app.workers()` 返回 `WebWorkerHandle` 列表。
- `WebWorkerContext::redis()` → `RedisHandle`；`WebWorkerContext::db()` → `DbHandle`（pool 化）。
- `RedisHandle::command(std::span<const std::string_view> args)` → `ScopedOperation<RedisValue>`
  （可 `co_await`）：发任意命令（XADD/XREADGROUP/XACK/XDEL/XGROUP…）并解析 `RedisValue`。
  另有 typed helper：`hgetAll/hset/hdel/del/incr/expire/exists/eval/evalSha/scriptLoad/pipeline/transaction`。
- `DbHandle`：`co_await ctx.db().query(...)`（与北向 `c.db()` 同款）。
- 定时/退避：worker 上的 asio `steady_timer`（`co_await` 异步等待），取代 `std::this_thread::sleep_for`。

## 统一架构

```
ruvia App (asio io_context, N workers)
  └─ onStart：
       - 取一个/一组 WebWorkerHandle 作为南桥后台执行器
       - worker.post(consumeConfig 协程)   —— co_await ctx.db()/ctx.redis()
       - worker.post(consumeProtocol 协程) —— co_await ctx.redis()
       - TcpLinkManager 绑定到**同一** worker 的 asio io_context
            → 网络回调、producer 发布、消费者协程共享一个执行器，全部可 co_await
```

关键点：**tcp_link.manager 复用 ruvia worker 的 asio io_context**（不再自建线程/ioContext）。
这样 socket 读回调解析出报文后可直接 `co_await producer.publish(...)`，producer 也异步化。

## 逐文件改法

### `queue/redis_stream.h`（同步 hiredis → async helper）
- 现状：`RedisStreamConnection` 自持 hiredis 连接，同步 `command()`，~20 个方法
  （`readGroup/ensureGroup/acknowledgeGroupedAndDelete/addGroupedBounded/hashEntries/claimHash/
  setHash/eraseHash/incrementWithExpiry/keysMatching/completeInflightTask/erase/...`）。
- 改法：删除自持连接；改为一组 **async 自由函数/薄封装**，签名 `Task<T> op(RedisHandle redis, ...)`，
  内部 `co_await redis.command({...})` 并解析 `RedisValue`。流命令用通用 `command`；
  hash/del/expire 用 typed helper。`claimHash`/`completeInflightTask` 等原子操作用 `eval`（Lua）保持原语义。
- `RedisStreamProducer`（`publish/updateSession/removeSession/clearSessions/writeLinkStatus/…`）
  去掉 `std::mutex`，改成 `Task<...>`，`co_await redis.command(...)`。调用方（dispatcher/linkManager）改为 `co_await`。

### `runtime_config.repository.h`（同步 libpq → async DB）
- 现状：`RuntimeConfigRepository` 用同步 libpq（`PQntuples` 等）`load()`。
- 改法：`Task<RuntimeSnapshot> load(DbHandle db)`，内部 `co_await db.query(...)`，
  行解析改用 ruvia QueryResult（与北向 service 同款）。SQL 与字段保持不变。

### `southbridge.runtime.h`（线程 → 协程）
- `start()`：不再建 `std::thread`；改为 `worker.post([this](WebWorkerContext& ctx) -> Task<void> { co_await consumeConfig(ctx); ... })`。
  启动握手（`std::promise/future` 传 counts）改为协程内顺序 `co_await`。
- `consumeProtocol(ctx)` / `consumeConfig(ctx)`：`while (running_)` 循环体全部 `co_await`；
  `redis.readGroup(...)` → `co_await` async 版本；退避 `sleep_for` → `co_await steadyTimer(...)`。
- `executeProtocolTask` 及 inflight/timeout/failure 处理：全部改 `Task<...>` + `co_await`，逻辑与错误分支逐条保留。
- `linkManager_.send(...)` future → `co_await` awaitable（见下）。

### `network/tcp_link.manager.h`（future → awaitable，复用 ruvia io_context）
- 构造时接收 ruvia worker 的 `asio::io_context&`（由 runtime 传入），不再自建。
- `send(connectionId, bytes)` 由返回 `std::future<bool>` 改为返回 asio awaitable
  （`asio::awaitable<bool>` 或 ruvia `Task<bool>`），内部 `async_write` 用 `co_await`（`use_awaitable`）。
- 网络读回调 → `dispatcher_.process` → `producer_.publish` 链路改为在 io_context 上 `co_await` 异步发布。

### 北桥（能用就行，最小处理）
- `LinkService::fetchPublicIp()` 的同步阻塞 socket：非本次重点，记为待办；不影响北桥可用。

## 里程碑与验证点

- **M1｜Redis/DB 异步基座**：redis_stream async helper + repository async。
  *验证*：单元/手测某几个 helper 对 Redis 的读写结果与旧同步版一致；config 快照 `load` 数量正确。
- **M2｜消费者协程化**：consumeConfig/consumeProtocol 跑成 worker 协程，退避用 steady_timer。
  *验证*：启动无死锁；config 事件触发重载；协议任务/响应匹配/超时重试/死信路径逐条走通。
- **M3｜producer + 网络异步统一**：tcp_link.manager 复用 ruvia io_context，send/publish 全异步。
  *验证*：主动上报设备的报文 → 遥测入流；查询—响应设备命令下发 → 应答匹配；连接建立/断开/重连不丢消息。

每个里程碑：`cmake --build build` 绿 + 上述运行时验证通过后再进入下一个。

## 非目标
- 不改消息契约、流名、消费组名、Redis key 结构、错误/死信语义。
- 不改设备协议解析（dispatcher/adapter 内部逻辑）。
- 不改北桥业务逻辑（仅记录 fetchPublicIp 待办）。
