#include "puls/gui/settings_store.hpp"

namespace puls::gui {

namespace {

QString key_name(std::string_view key) {
    return QString::fromUtf8(key.data(), static_cast<qsizetype>(key.size()));
}

} // namespace

SettingsStore::SettingsStore() : settings_(std::make_unique<QSettings>()) {}

SettingsStore::SettingsStore(const QString& path)
    : settings_(std::make_unique<QSettings>(path, QSettings::IniFormat)) {}

std::optional<std::string> SettingsStore::text(std::string_view key) const {
    const QVariant value = settings_->value(key_name(key));
    if (!value.isValid()) {
        return std::nullopt;
    }
    return value.toString().toStdString();
}

std::optional<double> SettingsStore::number(std::string_view key) const {
    const QVariant value = settings_->value(key_name(key));
    bool valid = false;
    const double result = value.toDouble(&valid);
    if (!value.isValid() || !valid) {
        return std::nullopt;
    }
    return result;
}

void SettingsStore::set_text(std::string_view key, std::string_view value) {
    settings_->setValue(key_name(key),
                        QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())));
}

void SettingsStore::set_number(std::string_view key, double value) {
    settings_->setValue(key_name(key), value);
}

} // namespace puls::gui
