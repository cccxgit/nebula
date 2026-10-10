class BatchHolder : public boost::noncopyable, public nebula::cpp::NonMovable {
 public:
  BatchHolder() = default;
  ~BatchHolder() = default;

  /**
   * @brief Add a put operation to batch
   *
   * @param key Key to put
   * @param val Value to put
   */
  void put(std::string&& key, std::string&& val) {
    size_ += key.size() + val.size();
    auto op = std::make_tuple(
        BatchLogType::OP_BATCH_PUT, std::forward<std::string>(key), std::forward<std::string>(val));
    batch_.emplace_back(std::move(op));
  }

  /**
   * @brief Add a remove operation to batch
   *
   * @param key Key to remove
   */
  void remove(std::string&& key) {
    size_ += key.size();
    auto op = std::make_tuple(BatchLogType::OP_BATCH_REMOVE, std::forward<std::string>(key), "");
    batch_.emplace_back(std::move(op));
  }

  /**
   * @brief Add a remove range operation to batch, [start, end)
   *
   * @param begin Start key to remove
   * @param end End key to remove
   */
  void rangeRemove(std::string&& begin, std::string&& end) {
    size_ += begin.size() + end.size();
    auto op = std::make_tuple(BatchLogType::OP_BATCH_REMOVE_RANGE,
                              std::forward<std::string>(begin),
                              std::forward<std::string>(end));
    batch_.emplace_back(std::move(op));
  }

  /**
   * @brief reserve spaces for batch
   */
  void reserve(int32_t size) {
    batch_.reserve(size);
  }

  /**
   * @brief Get the batch object
   *
   * @return const std::vector<std::tuple<BatchLogType, std::string, std::string>>&
   */
  const std::vector<std::tuple<BatchLogType, std::string, std::string>>& getBatch() const {
    return batch_;
  }

  /**
   * @brief size of key in operation of the batch, in bytes
   */
  size_t size() {
    return size_;
  }

 private:
  std::vector<std::tuple<BatchLogType, std::string, std::string>> batch_;
  size_t size_{0};
};
