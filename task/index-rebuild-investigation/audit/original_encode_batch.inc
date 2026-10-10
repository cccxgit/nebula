std::string encodeBatchValue(
    const std::vector<std::tuple<BatchLogType, std::string, std::string>>& batch) {
  auto type = LogType::OP_BATCH_WRITE;
  std::string encoded;
  encoded.reserve(1024);

  // Timestamp (8 bytes)
  int64_t ts = time::WallClock::fastNowInMilliSec();
  encoded.append(reinterpret_cast<char*>(&ts), sizeof(int64_t));
  // Log type
  encoded.append(reinterpret_cast<char*>(&type), 1);
  // Number of values
  auto num = static_cast<uint32_t>(batch.size());
  encoded.append(reinterpret_cast<char*>(&num), sizeof(uint32_t));
  // Values
  for (auto& op : batch) {
    auto opType = std::get<0>(op);
    auto key = std::get<1>(op);
    auto val = std::get<2>(op);
    auto keySize = key.size();
    auto valSize = val.size();
    encoded.append(reinterpret_cast<char*>(&opType), 1)
        .append(reinterpret_cast<char*>(&keySize), sizeof(uint32_t))
        .append(key.data(), keySize)
        .append(reinterpret_cast<char*>(&valSize), sizeof(uint32_t))
        .append(val.data(), valSize);
  }

  return encoded;
}
