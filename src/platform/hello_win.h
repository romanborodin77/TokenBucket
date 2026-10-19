#pragma once
#include <QString>
namespace tb::platform::hello {
enum class Availability { Available, NotConfigured, Unavailable };
enum class Result { Verified, Failed, Canceled };
Availability availability();
Result request(void* hwnd, const QString& prompt);
}
