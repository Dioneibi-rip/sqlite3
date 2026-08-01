class Database;
class QueuedAsyncWorker;
class MaintenanceWorker;
class WriteCoordinator {
public:
	~WriteCoordinator();
	static std::shared_ptr<WriteCoordinator> ForFile(Napi::Env env, const std::string& file_id);
	void Enqueue(QueuedAsyncWorker* worker);
	void Finish(QueuedAsyncWorker* worker);
	const std::string& FileId() const { return file_id; }

private:
	friend class MaintenanceWorker;
	WriteCoordinator(Napi::Env env, std::string file_id);
	static void OnTimer(uv_timer_t* handle);
	static void OnTimerClosed(uv_handle_t* handle);
	void Flush();
	void CancelIdleTimer();
	void StartIdleTimer(QueuedAsyncWorker* worker);
	void DispatchMaintenance();
	void MaintenanceFinished(QueuedAsyncWorker* worker);

	std::string file_id;
	uv_loop_t* loop;
	uv_timer_t timer;
	std::deque<QueuedAsyncWorker*> queue;
	Napi::Env env;
	bool active;
	bool timer_active;
	bool maintenance_active;
	bool closing;
	bool idle_timer_pending;
	Database* idle_db;
	Napi::ObjectReference idle_owner;
};
