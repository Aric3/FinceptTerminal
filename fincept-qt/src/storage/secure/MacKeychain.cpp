#include "storage/secure/MacKeychain.h"

#include "core/config/EditionPolicy.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <memory>
#include <type_traits>

namespace fincept::mac_keychain {
namespace {
struct CFDeleter {
    void operator()(const void* value) const {
        if (value)
            CFRelease(value);
    }
};
template <typename T>
using CFPtr = std::unique_ptr<std::remove_pointer_t<T>, CFDeleter>;
CFPtr<CFStringRef> cf_string(const QString& value) {
    const auto utf8 = value.toUtf8();
    return CFPtr<CFStringRef>(CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(utf8.constData()),
                                                      utf8.size(), kCFStringEncodingUTF8, false));
}
CFPtr<CFMutableDictionaryRef> query(const QString& account) {
    CFPtr<CFMutableDictionaryRef> result(
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    const auto service = cf_string(QString::fromLatin1(edition::data_directory()) + ".credential-key");
    const auto name = cf_string(account);
    CFDictionarySetValue(result.get(), kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(result.get(), kSecAttrService, service.get());
    CFDictionarySetValue(result.get(), kSecAttrAccount, name.get());
    // Login Keychain supports locally signed apps without access-group entitlements.
    // The OS may request permission after an ad-hoc signed app is rebuilt.
    // Denial propagates to callers; never broaden the item's access control.
    return result;
}
std::string error(OSStatus status) {
    return QString("Keychain access failed (OSStatus %1)").arg(status).toStdString();
}
} // namespace
Result<void> add(const QString& account, const QByteArray& key) {
    if (account.isEmpty() || key.size() != 32)
        return Result<void>::err("Invalid credential key");
    auto attributes = query(account);
    const auto label = cf_string(QString::fromLatin1(edition::application()) + " credential encryption key");
    CFPtr<CFDataRef> data(CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(key.constData()), key.size()));
    CFDictionarySetValue(attributes.get(), kSecAttrLabel, label.get());
    CFDictionarySetValue(attributes.get(), kSecValueData, data.get());
    const auto status = SecItemAdd(attributes.get(), nullptr);
    return status == errSecSuccess ? Result<void>::ok() : Result<void>::err(error(status));
}
Result<QByteArray> read(const QString& account) {
    if (account.isEmpty())
        return Result<QByteArray>::err("Invalid Keychain account");
    auto attributes = query(account);
    CFDictionarySetValue(attributes.get(), kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(attributes.get(), kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef raw = nullptr;
    const auto status = SecItemCopyMatching(attributes.get(), &raw);
    CFPtr<CFTypeRef> data(raw);
    if (status != errSecSuccess)
        return Result<QByteArray>::err(error(status));
    if (!data || CFGetTypeID(data.get()) != CFDataGetTypeID())
        return Result<QByteArray>::err("Invalid Keychain data");
    const auto bytes = static_cast<CFDataRef>(data.get());
    if (CFDataGetLength(bytes) != 32)
        return Result<QByteArray>::err("Invalid Keychain key length");
    return Result<QByteArray>::ok(QByteArray(reinterpret_cast<const char*>(CFDataGetBytePtr(bytes)), 32));
}
Result<void> remove(const QString& account) {
    if (account.isEmpty())
        return Result<void>::err("Invalid Keychain account");
    auto attributes = query(account);
    const auto status = SecItemDelete(attributes.get());
    return status == errSecSuccess || status == errSecItemNotFound ? Result<void>::ok()
                                                                   : Result<void>::err(error(status));
}
} // namespace fincept::mac_keychain
