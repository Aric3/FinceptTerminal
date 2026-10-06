#pragma once

#include <QString>
#include <QUrl>

namespace fincept::edition {

// Build policy is separate from authentication and subscription state.
inline constexpr bool is_local() {
#ifdef FINCEPT_LOCAL_EDITION
    return true;
#else
    return false;
#endif
}

inline constexpr const char* organization() {
    return is_local() ? "Aric" : "Fincept";
}
inline constexpr const char* application() {
    return is_local() ? "FinceptLocal" : "FinceptTerminal";
}
inline constexpr const char* data_directory() {
    return is_local() ? "dev.aric.fincept.local" : "com.fincept.terminal";
}

inline bool is_official_service(const QUrl& url) {
    const QString host = url.host().toLower();
    return host == QLatin1String("fincept.in") || host.endsWith(QLatin1String(".fincept.in"));
}

inline bool allows_url(const QUrl& url) {
    return !is_local() || !is_official_service(url);
}

inline bool allows_screen(const QString& id) {
    return !is_local() || (id != QLatin1String("profile") && id != QLatin1String("forum") &&
                           id != QLatin1String("support") && id != QLatin1String("crypto_center"));
}

} // namespace fincept::edition
