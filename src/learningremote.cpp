#include <pigpio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

std::vector<Frame> loadDb(const std::string& path) {
  std::ifstream in(path);
  std::vector<Frame> frames;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::istringstream row(line);
    Frame frame;
    std::string durations;
    if (!std::getline(row, frame.name, '|') || !std::getline(row, durations)) {
      continue;
    }
    std::istringstream values(durations);
    std::string value;
    while (std::getline(values, value, ',')) {
      try {
        const auto n = std::stoul(value);
        if (n > 0 && n <= 1000000) frame.durations.push_back(static_cast<unsigned>(n));
      } catch (...) {
        frame.durations.clear();
        break;
      }
    }
    if (validName(frame.name) && frame.durations.size() >= 4) {
      frames.push_back(std::move(frame));
    }
  }
  return frames;
}

void saveFrame(const std::string& path, const Frame& frame) {
  auto frames = loadDb(path);
  auto it = std::find_if(frames.begin(), frames.end(), [&](const Frame& f) {
    return f.name == frame.name;
  });
  if (it == frames.end()) {
    frames.push_back(frame);
  } else {
    *it = frame;
  }
  const std::string temporary = path + ".tmp";
  std::ofstream out(temporary, std::ios::trunc);
  if (!out) {
    throw std::runtime_error("データベースを書き込めません: " + temporary);
  }
  out << "# learningremote capture v1; name|durations_us (receiver LOW mark, HIGH space)\n";
  for (const auto& item : frames) {
    out << item.name << '|';
    for (std::size_t i = 0; i < item.durations.size(); ++i) {
      if (i) out << ',';
      out << item.durations[i];
    }
    out << '\n';
  }
  out.close();
  if (!out) {
    throw std::runtime_error("データベースの保存に失敗しました");
  }
  if (std::rename(temporary.c_str(), path.c_str()) != 0) {
      throw std::runtime_error("一時ファイルをデータベースに置き換えられません");
  }
}

void usage(const char* exe) {
  std::cerr << "使い方:\n"
            << "  " << exe << " learn <name> [--db FILE] [--timeout SEC]  (受信 GPIO " << kReceiveGpio << ")\n"
            << "  " << exe << " send <name> [--db FILE] [--repeat N] [--gap MS]  (送信 GPIO "
            << kTransmitGpio1 << ", " << kTransmitGpio2 << ", " << kTransmitGpio3 << " 同時)\n"
            << "  " << exe << " list [--db FILE]\n";
}

int number(const std::string& s, const char* label, int min, int max) {
  std::size_t used = 0;
  int n;
  try {
    n = std::stoi(s, &used);
  } catch (...) {
    throw std::runtime_error(std::string(label) + " が不正です");
  }
  if (used != s.size() || n < min || n > max) {
    throw std::runtime_error(std::string(label) + " が範囲外です");
  }
  return n;
}

int getOption(int argc, char** argv, const std::string& key, int fallback, int min, int max) {
  for (int i = 3; i + 1 < argc; ++i) {
    if (argv[i] == key) {
      return number(argv[i + 1], key.c_str(), min, max);
    }
  }
  return fallback;
}

std::string getStringOption(int argc, char** argv, const std::string& key, const std::string& fallback) {
  for (int i = 2; i + 1 < argc; ++i) {
    if (argv[i] == key) {
      return argv[i + 1];
    }
  }
  return fallback;
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

int run(int argc, char** argv) {
  if (argc < 2) {
    usage(argv[0]); return 2;
  }
  const std::string command = argv[1];
  const std::string dbPath = getStringOption(argc, argv, "--db", "codes.txt");
  if (command == "list") {
    const auto frames = loadDb(dbPath);
    if (frames.empty()) {
      std::cout << "登録された信号はありません。\n"; return 0;
    }
    for (const auto& frame : frames) {
      std::cout << frame.name << " (" << frame.durations.size() << " segments)\n";
    }
    return 0;
  }
  if (command != "learn" && command != "send") {
    usage(argv[0]);
    return 2;
  }

  if (argc < 3) {
    usage(argv[0]);
    return 2;
  }

  const std::string name = argv[2];
  if (!validName(name)) {
    throw std::runtime_error("信号名に | や改行は使えません");
  }

  const bool learning = command == "learn";

  if (gpioInitialise() < 0) {
    throw std::runtime_error("pigpio に接続できません。pigpiod を起動してください");
  }

  int result = 0;
  try {
    if (learning) {
      const int timeout = getOption(argc, argv, "--timeout", 30, 1, 600);
      Capture capture;
      gpioSetMode(kReceiveGpio, PI_INPUT);
      gpioSetPullUpDown(kReceiveGpio, PI_PUD_OFF);
      gpioGlitchFilter(kReceiveGpio, 80);
      gpioSetAlertFuncEx(kReceiveGpio, alert, &capture);
      gpioSetWatchdog(kReceiveGpio, 20);
      std::cout << "GPIO " << kReceiveGpio << " で受信中です。リモコンのボタンを押してください (最大 " << timeout << " 秒)。\n";
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
      while (!interrupted && std::chrono::steady_clock::now() < deadline) {
        {
          std::lock_guard<std::mutex> lock(capture.mutex); if (capture.finished) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      gpioSetAlertFuncEx(kReceiveGpio, nullptr, nullptr);
      gpioSetWatchdog(kReceiveGpio, 0);
      std::vector<unsigned> captured;
      {
        std::lock_guard<std::mutex> lock(capture.mutex); captured = capture.durations;
      }
      if (interrupted) {
        throw std::runtime_error("中断されました");
      }
      if (captured.size() < 4) {
        throw std::runtime_error("信号を受信できませんでした。配線と GPIO 番号を確認してください");
      }
      Frame frame{name, std::move(captured)};
      saveFrame(dbPath, frame);
      std::cout << "「" << name << "」を " << dbPath << " に保存しました (" << frame.durations.size() << " segments)。\n";
    } else {
      const int repeat = getOption(argc, argv, "--repeat", 1, 1, 20);
      const int gap = getOption(argc, argv, "--gap", 100, 20, 2000);
      const auto frames = loadDb(dbPath);
      const auto it = std::find_if(frames.begin(), frames.end(), [&](const Frame& f) { return f.name == name; });
      if (it == frames.end()) {
        throw std::runtime_error("信号が見つかりません: " + name);
      }
      transmit(*it, repeat, gap);
      std::cout << "「" << name << "」を送信しました。\n";
    }
  } catch (...) {
    gpioTerminate();
    throw;
  }
  gpioTerminate();
  return result;
}
}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, onSignal);
  try {
    return run(argc, argv);
  }
  catch (const std::exception& e) {
    std::cerr << "エラー: " << e.what() << '\n';
    return 1;
  }
}
