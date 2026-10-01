#pragma once

#include "puls/gui/model.hpp"

#include <QSettings>

#include <memory>

namespace puls::gui {

// Preferences in the platform's settings storage: the registry on Windows,
// a property list on macOS and an INI file elsewhere.
class SettingsStore final : public PreferenceStore {
public:
    // Uses the application's organization and name.
    SettingsStore();
    // Uses an INI file, for tests.
    explicit SettingsStore(const QString& path);

    [[nodiscard]] std::optional<std::string> text(std::string_view key) const override;
    [[nodiscard]] std::optional<double> number(std::string_view key) const override;
    void set_text(std::string_view key, std::string_view value) override;
    void set_number(std::string_view key, double value) override;

private:
    std::unique_ptr<QSettings> settings_;
};

} // namespace puls::gui
