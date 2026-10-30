package hu.balazs.mirrortv;

import android.content.Context;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.util.Log;

/**
 * Advertises the AirPlay (_airplay._tcp) and AirTunes (_raop._tcp) services
 * through Android's built-in mDNS responder, so iPhones see the projector
 * in the Screen Mirroring list.
 */
final class MdnsAdvertiser {

    private static final String TAG = "MirrorTV-mdns";
    static final int SERVICE_RAOP = 0;
    static final int SERVICE_AIRPLAY = 1;

    private final NsdManager nsd;
    private final NsdManager.RegistrationListener[] listeners = new NsdManager.RegistrationListener[2];

    MdnsAdvertiser(Context context) {
        nsd = (NsdManager) context.getApplicationContext().getSystemService(Context.NSD_SERVICE);
    }

    synchronized void register(int service, String name, int port, String[] keys, String[] values) {
        unregister(service);

        NsdServiceInfo info = new NsdServiceInfo();
        info.setServiceName(name);
        info.setServiceType(service == SERVICE_RAOP ? "_raop._tcp" : "_airplay._tcp");
        info.setPort(port);
        for (int i = 0; i < keys.length; i++) {
            try {
                info.setAttribute(keys[i], values[i]);
            } catch (IllegalArgumentException e) {
                Log.w(TAG, "TXT attribute rejected: " + keys[i], e);
            }
        }

        NsdManager.RegistrationListener l = new NsdManager.RegistrationListener() {
            @Override
            public void onServiceRegistered(NsdServiceInfo s) {
                Log.i(TAG, "registered " + s.getServiceName() + " " + s.getServiceType());
            }

            @Override
            public void onRegistrationFailed(NsdServiceInfo s, int errorCode) {
                Log.e(TAG, "registration failed for " + s.getServiceType() + ": " + errorCode);
            }

            @Override
            public void onServiceUnregistered(NsdServiceInfo s) {
                Log.i(TAG, "unregistered " + s.getServiceType());
            }

            @Override
            public void onUnregistrationFailed(NsdServiceInfo s, int errorCode) {
                Log.w(TAG, "unregistration failed: " + errorCode);
            }
        };
        listeners[service] = l;
        nsd.registerService(info, NsdManager.PROTOCOL_DNS_SD, l);
    }

    synchronized void unregister(int service) {
        NsdManager.RegistrationListener l = listeners[service];
        if (l != null) {
            listeners[service] = null;
            try {
                nsd.unregisterService(l);
            } catch (IllegalArgumentException ignored) {
                // was never registered successfully
            }
        }
    }

    synchronized void unregisterAll() {
        unregister(SERVICE_RAOP);
        unregister(SERVICE_AIRPLAY);
    }
}
