class IndexCountWrapper {
 public:
  explicit IndexCountWrapper(StorageEnv* env) : count_(&env->onFlyingRequest_) {
    count_->fetch_add(1, std::memory_order_release);
  }

  IndexCountWrapper(const IndexCountWrapper&) = delete;

  IndexCountWrapper(IndexCountWrapper&& icw) noexcept : count_(icw.count_) {
    count_->fetch_add(1, std::memory_order_release);
  }

  IndexCountWrapper& operator=(const IndexCountWrapper&) = delete;

  IndexCountWrapper& operator=(IndexCountWrapper&& icw) noexcept {
    if (this != &icw) {
      count_ = icw.count_;
      count_->fetch_add(1, std::memory_order_release);
    }
    return *this;
  }

  ~IndexCountWrapper() {
    count_->fetch_sub(1, std::memory_order_release);
  }

 private:
  std::atomic<int32_t>* count_;
};
