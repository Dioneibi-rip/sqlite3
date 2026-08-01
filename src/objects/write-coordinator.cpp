static std::mutex write_coordinator_mutex;
static std::unordered_map<std::string, std::weak_ptr<WriteCoordinator>> write_coordinators;

class MaintenanceWorker : public QueuedAsyncWorker {
public:
	MaintenanceWorker(Napi::Env env, Database* db, Napi::Object owner, WriteCoordinator* coordinator)
		: QueuedAsyncWorker(env, db, owner), coordinator(coordinator) {}

	void Execute() override {
		sqlite3* const db_handle = db->GetHandle();
		char* error = NULL;
		int status = sqlite3_exec(db_handle, "PRAGMA wal_checkpoint(TRUNCATE);", NULL, NULL, &error);
		if (error != NULL) sqlite3_free(error);
		if (status == SQLITE_BUSY || status == SQLITE_LOCKED) return;
		error = NULL;
		status = sqlite3_exec(db_handle, "PRAGMA optimize;", NULL, NULL, &error);
		if (error != NULL) sqlite3_free(error);
		((void)status);
	}

	void OnOK() override {
		FinishQueue();
		coordinator->MaintenanceFinished(this);
	}

	void OnError(const Napi::Error&) override {
		FinishQueue();
		coordinator->MaintenanceFinished(this);
	}

private:
	WriteCoordinator* coordinator;
};

std::shared_ptr<WriteCoordinator> WriteCoordinator::ForFile(Napi::Env env, const std::string& file_id) {
	std::lock_guard<std::mutex> lock(write_coordinator_mutex);
	auto found = write_coordinators.find(file_id);
	if (found != write_coordinators.end()) {
		std::shared_ptr<WriteCoordinator> existing = found->second.lock();
		if (existing) return existing;
	}
	std::shared_ptr<WriteCoordinator> coordinator(new WriteCoordinator(env, file_id));
	write_coordinators[file_id] = coordinator;
	return coordinator;
}

WriteCoordinator::WriteCoordinator(Napi::Env env, std::string file_id) :
	file_id(std::move(file_id)),
	loop(NULL),
	timer(),
	queue(),
	env(env),
	active(false),
	timer_active(false),
	maintenance_active(false),
	closing(false),
	idle_timer_pending(false),
	idle_db(NULL),
	idle_owner() {
	napi_get_uv_event_loop(env, &loop);
	uv_timer_init(loop, &timer);
	timer.data = this;
}

WriteCoordinator::~WriteCoordinator() {
	closing = true;
	uv_timer_stop(&timer);
	if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&timer))) uv_close(reinterpret_cast<uv_handle_t*>(&timer), WriteCoordinator::OnTimerClosed);
}

void WriteCoordinator::Enqueue(QueuedAsyncWorker* worker) {
	CancelIdleTimer();
	queue.push_back(worker);
	if (active || timer_active) return;
	timer_active = true;
	uv_timer_start(&timer, WriteCoordinator::OnTimer, 5, 0);
}

void WriteCoordinator::Finish(QueuedAsyncWorker* worker) {
	active = false;
	Flush();
	if (!active && queue.empty()) StartIdleTimer(worker);
}

void WriteCoordinator::OnTimer(uv_timer_t* handle) {
	WriteCoordinator* coordinator = static_cast<WriteCoordinator*>(handle->data);
	coordinator->timer_active = false;
	if (coordinator->idle_timer_pending) {
		coordinator->idle_timer_pending = false;
		coordinator->DispatchMaintenance();
	} else {
		coordinator->Flush();
	}
}

void WriteCoordinator::OnTimerClosed(uv_handle_t* handle) {
	handle->data = NULL;
}

void WriteCoordinator::Flush() {
	if (closing || active || queue.empty()) return;
	QueuedAsyncWorker* worker = queue.front();
	queue.pop_front();
	active = true;
	worker->MarkWriteCoordinated();
	worker->GetDatabase()->EnqueueAsync(worker);
}

void WriteCoordinator::CancelIdleTimer() {
	if (!idle_timer_pending) return;
	uv_timer_stop(&timer);
	idle_timer_pending = false;
	idle_owner.Reset();
	idle_db = NULL;
}

void WriteCoordinator::StartIdleTimer(QueuedAsyncWorker* worker) {
	if (closing || maintenance_active || timer_active || idle_timer_pending) return;
	idle_db = worker->GetDatabase();
	idle_owner.Reset(worker->Owner(), 1);
	idle_timer_pending = true;
	uv_timer_start(&timer, WriteCoordinator::OnTimer, 15000, 0);
}

void WriteCoordinator::DispatchMaintenance() {
	if (closing || active || maintenance_active || !queue.empty() || idle_db == NULL || idle_owner.IsEmpty()) return;
	maintenance_active = true;
	MaintenanceWorker* worker = new MaintenanceWorker(env, idle_db, idle_owner.Value(), this);
	idle_owner.Reset();
	idle_db->EnqueueAsync(worker);
}

void WriteCoordinator::MaintenanceFinished(QueuedAsyncWorker* worker) {
	maintenance_active = false;
	if (!closing && !active && queue.empty()) StartIdleTimer(worker);
}
