#include "CursorUsageFetcher.h"
#include "CursorUsageOptions.h"
#include "CursorUsageVersion.h"

#include "PluginInterface.h"

#include <chrono>
#include <mutex>
#include <string>
#include <thread>

namespace {

class CursorUsageItem final : public IPluginItem {
public:
    enum class Kind {
        User,
        CursorModels,
        OtherModels,
    };

    CursorUsageItem(Kind kind, const wchar_t* name, const wchar_t* id, const wchar_t* label,
                    const wchar_t* sampleText)
        : kind_(kind), name_(name), id_(id), label_(label), sampleText_(sampleText) {}

    const wchar_t* GetItemName() const override { return name_; }
    const wchar_t* GetItemId() const override { return id_; }
    const wchar_t* GetItemLableText() const override { return label_; }
    const wchar_t* GetItemValueText() const override { return valueText_.c_str(); }
    const wchar_t* GetItemValueSampleText() const override { return sampleText_; }
    int IsDrawResourceUsageGraph() const override { return kind_ == Kind::User ? 0 : 1; }
    float GetResourceUsageGraphValue() const override {
        if (kind_ == Kind::User || remainingPercent_ < 0) {
            return 0.0f;
        }
        return static_cast<float>(remainingPercent_) / 100.0f;
    }

    void SetSnapshot(const UsageSnapshot& snapshot) {
        if (kind_ == Kind::User) {
            valueText_ = FormatUserDisplay(snapshot);
            remainingPercent_ = -1;
            return;
        }

        if (!snapshot.success) {
            remainingPercent_ = -1;
            valueText_ = L"--";
            return;
        }

        remainingPercent_ = kind_ == Kind::CursorModels
            ? snapshot.cursorModels.remainingPercent
            : snapshot.otherModels.remainingPercent;
        valueText_ = FormatRemainingPercent(remainingPercent_);
    }

private:
    Kind kind_;
    const wchar_t* name_;
    const wchar_t* id_;
    const wchar_t* label_;
    const wchar_t* sampleText_;
    int remainingPercent_ = -1;
    std::wstring valueText_ = L"--";
};

class CursorUsagePlugin final : public ITMPlugin {
public:
    static CursorUsagePlugin& Instance() {
        static CursorUsagePlugin plugin;
        return plugin;
    }

    IPluginItem* GetItem(int index) override {
        if (index == 0) {
            return &userItem_;
        }
        if (index == 1) {
            return &cursorModelsItem_;
        }
        if (index == 2) {
            return &otherModelsItem_;
        }
        return nullptr;
    }

    void DataRequired() override {
        const auto now = std::chrono::steady_clock::now();
        CursorUsageConfig config;
        unsigned long long generation;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (fetchInProgress_) {
                return;
            }
            if (lastFetch_.time_since_epoch().count() != 0 && now - lastFetch_ < refreshInterval_) {
                return;
            }
            fetchInProgress_ = true;
            lastFetch_ = now;
            config = config_;
            generation = configGeneration_;
        }

        std::thread([this, config, generation]() {
            UsageSnapshot snapshot = fetcher_.Fetch(config);
            std::lock_guard<std::mutex> lock(mutex_);
            fetchInProgress_ = false;
            if (generation != configGeneration_) return;
            snapshot_ = std::move(snapshot);
            ApplySnapshot(snapshot_);
            tooltip_ = BuildTooltip(snapshot_);
        }).detach();
    }

    OptionReturn ShowOptionsDialog(void* parent) override {
        CursorUsageConfig config;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            config = config_;
        }
        if (!ShowCursorOptions(reinterpret_cast<HWND>(parent), &config)) return OR_OPTION_UNCHANGED;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            config_ = std::move(config);
            ++configGeneration_;
            lastFetch_ = {};
            snapshot_ = {};
            ApplySnapshot(snapshot_);
            tooltip_ = L"Cursor Usage: waiting for data";
        }
        DataRequired();
        return OR_OPTION_CHANGED;
    }

    const wchar_t* GetInfo(PluginInfoIndex index) override {
        switch (index) {
            case TMI_NAME:
                return L"Cursor Usage";
            case TMI_DESCRIPTION:
                return L"Shows Cursor account, Models and Other Models remaining usage.";
            case TMI_AUTHOR:
                return L"CursorUsage";
            case TMI_COPYRIGHT:
                return L"MIT";
            case TMI_VERSION:
                return CURSOR_USAGE_VERSION_WIDE;
            case TMI_URL:
                return L"https://github.com/yhsj0919/TrafficMonitor_Cursor_Plugin";
            default:
                return L"";
        }
    }

    const wchar_t* GetTooltipInfo() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return tooltip_.c_str();
    }

private:
    CursorUsagePlugin() = default;

    void ApplySnapshot(const UsageSnapshot& snapshot) {
        userItem_.SetSnapshot(snapshot);
        cursorModelsItem_.SetSnapshot(snapshot);
        otherModelsItem_.SetSnapshot(snapshot);
    }

    std::wstring BuildTooltip(const UsageSnapshot& snapshot) const {
        return BuildUsageTooltip(snapshot);
    }

    CursorUsageConfig config_ = LoadCursorConfig();
    unsigned long long configGeneration_ = 0;
    CursorUsageFetcher fetcher_;
    CursorUsageItem userItem_{CursorUsageItem::Kind::User, L"Cursor User", L"CursorUsageUser", L"用户: ", L"user@example.com"};
    CursorUsageItem cursorModelsItem_{CursorUsageItem::Kind::CursorModels, L"Cursor Models Remaining", L"CursorUsageModels", L"模型: ", L"100%"};
    CursorUsageItem otherModelsItem_{CursorUsageItem::Kind::OtherModels, L"Other Models Remaining", L"CursorUsageOther", L"其它: ", L"100%"};
    mutable std::mutex mutex_;
    UsageSnapshot snapshot_;
    std::wstring tooltip_ = L"Cursor Usage: waiting for data";
    bool fetchInProgress_ = false;
    std::chrono::steady_clock::time_point lastFetch_{};
    static constexpr std::chrono::minutes refreshInterval_{1};
};

}  // namespace

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance() {
    return &CursorUsagePlugin::Instance();
}
