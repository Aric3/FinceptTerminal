#include "screens/settings/NetworkSection.h"

#include "network/ProxyManager.h"
#include "screens/settings/SettingsStyles.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace fincept::screens {
NetworkSection::NetworkSection(QWidget* parent) : QWidget(parent) {
    const auto settings = network::ProxySettings::load();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    auto* heading = new QLabel(tr("NETWORK & PROXY"));
    heading->setStyleSheet(settings_styles::section_title_ss());
    layout->addWidget(heading);
    auto* form = new QFormLayout;
    auto* mode = new QComboBox;
    mode->addItem(tr("System proxy"), static_cast<int>(network::ProxyMode::System));
    mode->addItem(tr("Manual HTTP proxy"), static_cast<int>(network::ProxyMode::Manual));
    mode->addItem(tr("Direct connection"), static_cast<int>(network::ProxyMode::Direct));
    mode->setCurrentIndex(mode->findData(static_cast<int>(settings.mode)));
    auto* host = new QLineEdit(settings.host);
    host->setPlaceholderText("127.0.0.1");
    auto* port = new QSpinBox;
    port->setRange(1, 65535);
    port->setValue(settings.port ? settings.port : 7897);
    auto* bypass = new QLineEdit(settings.bypass.join(','));
    form->addRow(tr("Mode"), mode);
    form->addRow(tr("Proxy host"), host);
    form->addRow(tr("Proxy port"), port);
    form->addRow(tr("Bypass hosts"), bypass);
    layout->addLayout(form);
    auto update = [mode, host, port] {
        const bool manual = mode->currentData().toInt() == static_cast<int>(network::ProxyMode::Manual);
        host->setEnabled(manual);
        port->setEnabled(manual);
    };
    connect(mode, &QComboBox::currentIndexChanged, this, update);
    update();
    auto* description =
        new QLabel(tr("Applies to this app only. Qt requests and Python downloads share this setting. "
                      "System mode supports static HTTP/HTTPS proxies. For PAC or SOCKS-only setups, "
                      "use your proxy app's HTTP port in manual mode. Loopback always connects directly. "
                      "Bypass hosts: exact names or .example.com suffixes, separated by commas."));
    description->setWordWrap(true);
    layout->addWidget(description);
    auto* save = new QPushButton(tr("Save proxy settings"));
    layout->addWidget(save);
    auto* status = new QLabel;
    status->setWordWrap(true);
    layout->addWidget(status);
    connect(save, &QPushButton::clicked, this, [mode, host, port, bypass, status] {
        network::ProxySettings config;
        config.mode = static_cast<network::ProxyMode>(mode->currentData().toInt());
        config.host = host->text().trimmed();
        config.port = static_cast<quint16>(port->value());
        config.bypass = bypass->text().split(',', Qt::SkipEmptyParts);
        for (auto& entry : config.bypass)
            entry = entry.trimmed();
        QString error;
        status->setText(config.save(&error)
                            ? QObject::tr("Saved. Restart the app to apply to all connections and workers.")
                            : error);
    });
    layout->addStretch();
}
} // namespace fincept::screens
