#include ../../security/manager/ssl/security-prefs.js
#include init/all.js
#ifdef MOZ_SERVICES_HEALTHREPORT
#if MOZ_WIDGET_TOOLKIT == android
#include ../../mobile/android/chrome/content/healthreport-prefs.js
#else
#include ../../toolkit/components/telemetry/healthreport-prefs.js
#endif
#endif
