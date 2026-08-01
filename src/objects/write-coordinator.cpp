static std::mutex write_coordinator_mutex;
static std::unordered_map<std::string, std::weak_ptr<WriteCoordinator>> write_coordinators;

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
	active(false),
	timer_active(false) {
	napi_get_uv_event_loop(env, &loop);
	uv_timer_init(loop, &timer);
	timer.data = this;
}

WriteCoordinator::~WriteCoordinator() {
	uv_timer_stop(&timer);
	if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&timer))) uv_close(reinterpret_cast<uv_handle_t*>(&timer), NULL);
}

void WriteCoordinator::Enqueue(QueuedAsyncWorker* worker) {
	queue.push_back(worker);
	if (active || timer_active) return;
	timer_active = true;
	uv_timer_start(&timer, WriteCoordinator::OnTimer, 5, 0);
}

void WriteCoordinator::Finish() {
	active = false;
	Flush();
}

void WriteCoordinator::OnTimer(uv_timer_t* handle) {
	WriteCoordinator* coordinator = static_cast<WriteCoordinator*>(handle->data);
	coordinator->timer_active = false;
	coordinator->Flush();
}

void WriteCoordinator::Flush() {
	if (active || queue.empty()) return;
	QueuedAsyncWorker* worker = queue.front();
	queue.pop_front();
	active = true;
	worker->MarkWriteCoordinated();
	worker->GetDatabase()->EnqueueAsync(worker);
}
