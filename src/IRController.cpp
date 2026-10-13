#include "IRController.h"
#include "IRFileAccessor.h"

#include <pigpio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sstream>

namespace {
constexpr unsigned kReceiveGpio = 17;
constexpr unsigned kTransmitGpio1 = 13;
constexpr unsigned kTransmitGpio2 = 19;
constexpr unsigned kTransmitGpio3 = 26;
constexpr unsigned kHalfCarrierUs = 13;  // Approximately 38.5 kHz.
constexpr std::size_t kMaxEdges = 20000;
std::atomic<bool> interrupted{false};

struct Frame {
  std::string name;
  std::vector<unsigned> durations;
};

void onSignal(int) {
  interrupted = true;
}

struct Capture {
  std::mutex mutex;
  std::vector<unsigned> durations;
  uint32_t lastTick = 0;
  int previousLevel = 1;
  bool started = false;
  bool finished = false;
};

void alert(int, int level, uint32_t tick, void* userdata) {
  auto* capture = static_cast<Capture*>(userdata);
  std::lock_guard<std::mutex> lock(capture->mutex);
  if (level == PI_TIMEOUT) {
    if (capture->started && !capture->durations.empty()) {
      capture->finished = true;
      return;
    }
    return;
  }

  if (!capture->started) {
    capture->started = true;
    capture->lastTick = tick;
    capture->previousLevel = level;
    return;
  }

  const uint32_t elapsed = tick - capture->lastTick;  // pigpio ticks wrap naturally.
  if (elapsed > 0 && elapsed < 1000000 && capture->durations.size() < kMaxEdges) {
    // IR receiver modules output LOW while the IR LED is on (mark).
    capture->durations.push_back(elapsed);
    capture->previousLevel = level;
    capture->lastTick = tick;
  } else if (capture->durations.size() >= kMaxEdges) {
    capture->finished = true;
  }
}

bool validName(const std::string& name) {
  return !name.empty() && name.find_first_of("\t\r\n|") == std::string::npos;
}

std::string encodeDurations(const std::vector<unsigned>& durations) {
  std::ostringstream output;
  for (std::size_t i = 0; i < durations.size(); ++i) {
    if (i) output << ',';
    output << durations[i];
  }
  return output.str();
}

std::vector<unsigned> decodeDurations(const std::string& encoded) {
  std::istringstream input(encoded);
  std::vector<unsigned> durations;
  std::string value;
  while (std::getline(input, value, ',')) {
    std::size_t used = 0;
    unsigned long duration;
    try {
      duration = std::stoul(value, &used);
    } catch (...) {
      throw std::runtime_error("データベース内の信号データが不正です");
    }
    if (used != value.size() || duration == 0 || duration > 1000000) {
      throw std::runtime_error("データベース内の信号データが不正です");
    }
    durations.push_back(static_cast<unsigned>(duration));
  }
  if (durations.size() < 4) {
    throw std::runtime_error("データベース内の信号データが不正です");
  }
  return durations;
}

int transmit(const Frame& frame, int repeat, int gapMs) {
  std::vector<gpioPulse_t> pulses;
  const uint32_t pin = (1u << kTransmitGpio1) | (1u << kTransmitGpio2) | (1u << kTransmitGpio3);
  for (int r = 0; r < repeat; ++r) {
    for (std::size_t i = 0; i < frame.durations.size(); ++i) {
      unsigned duration = frame.durations[i];
      const bool mark = (i % 2 == 0);
      if (!mark) {
        unsigned remaining = duration;
        while (remaining) {
          const unsigned part = std::min(remaining, 60000u);
          pulses.push_back({0, pin, part});
          remaining -= part;
        }
      } else {
        while (duration >= 2 * kHalfCarrierUs) {
          pulses.push_back({pin, 0, kHalfCarrierUs});
          pulses.push_back({0, pin, kHalfCarrierUs});
          duration -= 2 * kHalfCarrierUs;
        }
        if (duration) pulses.push_back({pin, 0, duration});
      }
      if (pulses.size() > 12000) {
        throw std::runtime_error("波形が長すぎます");
      }
    }
    if (r + 1 < repeat) {
      unsigned remaining = static_cast<unsigned>(gapMs) * 1000;
      while (remaining) {
        const unsigned part = std::min(remaining, 60000u);
        pulses.push_back({0, pin, part});
        remaining -= part;
      }
    }
  }
  gpioSetMode(kTransmitGpio1, PI_OUTPUT);
  gpioSetMode(kTransmitGpio2, PI_OUTPUT);
  gpioSetMode(kTransmitGpio3, PI_OUTPUT);
  gpioWrite(kTransmitGpio1, 0);
  gpioWrite(kTransmitGpio2, 0);
  gpioWrite(kTransmitGpio3, 0);
  if (gpioWaveAddGeneric(static_cast<unsigned>(pulses.size()), pulses.data()) < 0) {
    throw std::runtime_error("pigpio が送信波形を作成できませんでした");
  }

  const int waveId = gpioWaveCreate();
  if (waveId < 0) {
    throw std::runtime_error("pigpio の波形メモリが不足しています");
  }
  const int sent = gpioWaveTxSend(waveId, PI_WAVE_MODE_ONE_SHOT);
  if (sent < 0) {
    gpioWaveDelete(waveId);
    throw std::runtime_error("赤外線信号を送信できませんでした");
  }
  while (gpioWaveTxBusy() && !interrupted) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  gpioWaveTxStop();
  gpioWaveDelete(waveId);
  gpioWrite(kTransmitGpio1, 0);
  gpioWrite(kTransmitGpio2, 0);
  gpioWrite(kTransmitGpio3, 0);
  return 0;
}


}  // namespace


void IRController::list(const std::string& dbPath) const {
  IRFileAccessor accessor;
  accessor.open(dbPath);
  const auto records = accessor.fetchAll();
  accessor.commit();
  if (records.empty()) {
    std::cout << "登録された信号はありません。\n";
    return;
  }
  for (const auto& record : records) {
    const auto durations = decodeDurations(record.data);
    std::cout << record.name << " (" << durations.size() << " segments)\n";
  }
}

void IRController::clearAll(const std::string& dbPath) const {
  IRFileAccessor accessor;
  accessor.open(dbPath);
  accessor.clearAll();
  accessor.commit();
  std::cout << "「" << dbPath << "」の学習データをすべて削除しました。\n";
}

void IRController::deleteSignal(const std::string& name, const std::string& dbPath) const {
  IRFileAccessor accessor;
  accessor.open(dbPath);
  const bool deleted = accessor.deleteByName(name);
  accessor.commit();
  if (!deleted) {
    throw std::runtime_error("信号が見つかりません: " + name);
  }
  std::cout << "「" << name << "」を削除しました。\n";
}

void IRController::learn(const std::string& name, const std::string& dbPath, int timeoutSec) {
  if (!validName(name)) {
    throw std::runtime_error("信号名に | や改行は使えません");
  }
  if (gpioInitialise() < 0) {
    throw std::runtime_error("pigpio に接続できません。pigpiod を起動してください");
  }
  try {
    Capture capture;
    gpioSetMode(kReceiveGpio, PI_INPUT);
    gpioSetPullUpDown(kReceiveGpio, PI_PUD_OFF);
    gpioGlitchFilter(kReceiveGpio, 80);
    gpioSetAlertFuncEx(kReceiveGpio, alert, &capture);
    gpioSetWatchdog(kReceiveGpio, 20);
    std::cout << "GPIO " << kReceiveGpio << " で受信中です。リモコンのボタンを押してください (最大 "
              << timeoutSec << " 秒)。\n";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    std::cout << "interrupted: " << interrupted << std::endl;
    while (!interrupted && std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard<std::mutex> lock(capture.mutex);
        if (capture.finished) break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    gpioSetAlertFuncEx(kReceiveGpio, nullptr, nullptr);
    gpioSetWatchdog(kReceiveGpio, 0);
    std::vector<unsigned> captured;
    {
      std::lock_guard<std::mutex> lock(capture.mutex);
      captured = capture.durations;
    }
    if (interrupted) {
      throw std::runtime_error("中断されました");
    }
    if (captured.size() < 4) {
      throw std::runtime_error("信号を受信できませんでした。配線と GPIO 番号を確認してください");
    }

    IRFileAccessor accessor;
    accessor.open(dbPath);
    const std::string data = encodeDurations(captured);
    if (!accessor.update(name, data, 1, 100)) {
      accessor.insert(name, data, 1, 100);
    }
    accessor.commit();
    std::cout << "「" << name << "」を " << dbPath << " に保存しました ("
              << captured.size() << " segments)。\n";
  } catch (...) {
    gpioTerminate();
    throw;
  }
  gpioTerminate();
}

void IRController::send(const std::string& name, const std::string& dbPath, int repeat, int gapMs) {
  if (!validName(name)) {
    throw std::runtime_error("信号名に | や改行は使えません");
  }
  IRFileAccessor accessor;
  accessor.open(dbPath);
  const auto record = accessor.fetchByName(name);
  accessor.commit();
  if (!record) {
    throw std::runtime_error("信号が見つかりません: " + name);
  }
  if (gpioInitialise() < 0) {
    throw std::runtime_error("pigpio に接続できません。pigpiod を起動してください");
  }
  try {
    Frame frame{name, decodeDurations(record->data)};
    transmit(frame, repeat, gapMs);
    std::cout << "「" << name << "」を送信しました。\n";
  } catch (...) {
    gpioTerminate();
    throw;
  }
  gpioTerminate();
}

void IRController::installSignalHandlers() {
  std::signal(SIGINT, onSignal);
}

bool IRController::interrupt() const {
  return ::interrupted.load();
}
