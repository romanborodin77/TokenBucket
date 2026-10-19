#include "hello_win.h"
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Credentials.UI.h>
#include <UserConsentVerifierInterop.h>
namespace tb::platform::hello {
namespace {
struct Apartment {
    Apartment() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    ~Apartment() { winrt::uninit_apartment(); }
};
}
Availability availability() {
    try {
        Apartment apartment;
        using namespace winrt::Windows::Security::Credentials::UI;
        const auto result = UserConsentVerifier::CheckAvailabilityAsync().get();
        if (result == UserConsentVerifierAvailability::Available) return Availability::Available;
        if (result == UserConsentVerifierAvailability::DeviceNotPresent
            || result == UserConsentVerifierAvailability::NotConfiguredForUser) return Availability::NotConfigured;
    } catch (...) {}
    return Availability::Unavailable;
}
Result request(void* hwnd, const QString& prompt) {
    if (!hwnd || !IsWindow(static_cast<HWND>(hwnd))) return Result::Failed;
    try {
        Apartment apartment;
        using namespace winrt::Windows::Security::Credentials::UI;
        using Operation = winrt::Windows::Foundation::IAsyncOperation<UserConsentVerificationResult>;
        const auto interop = winrt::get_activation_factory<UserConsentVerifier, IUserConsentVerifierInterop>();
        Operation operation{nullptr};
        const winrt::hstring message(prompt.toStdWString());
        winrt::check_hresult(interop->RequestVerificationForWindowAsync(static_cast<HWND>(hwnd), static_cast<HSTRING>(winrt::get_abi(message)),
            winrt::guid_of<Operation>(), winrt::put_abi(operation)));
        const auto result = operation.get();
        if (result == UserConsentVerificationResult::Verified) return Result::Verified;
        if (result == UserConsentVerificationResult::Canceled) return Result::Canceled;
    } catch (const winrt::hresult_canceled&) { return Result::Canceled; }
      catch (...) {}
    return Result::Failed;
}
}
