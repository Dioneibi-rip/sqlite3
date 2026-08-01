class QueuedAsyncWorker;
class WriteCoordinator {
public:
	~WriteCoordinator();
	static std::shared_ptr<WriteCoordinator> ForFile(Napi::Env env, const std::string& file_id);
	void Enqueue(QueuedAsyncWorker* worker);
	void Finish();
	const std::string& FileId() const { return file_id; }

private:
	WriteCoordinator(Napi::Env env, std::string file_id);
	static void OnTimer(uv_timer_t* handle);
	void Flush();

	std::string file_id;
	uv_loop_t* loop;
	uv_timer_t timer;
	std::deque<QueuedAsyncWorker*> queue;
	bool active;
	bool timer_active;
};
