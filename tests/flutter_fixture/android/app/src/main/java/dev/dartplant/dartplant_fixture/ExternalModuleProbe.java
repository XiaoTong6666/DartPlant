package dev.dartplant.dartplant_fixture;

/**
 * Deliberately tiny Java/Native rendezvous point for the external libxposed
 * integration tests. Production Dart AOT hooks use the independent C ABI.
 */
public final class ExternalModuleProbe {
    private static final int CALLBACK_ABI_VERSION = 2;
    private static volatile long bootstrapEntry;
    private static volatile long countersEntry;
    private static volatile long retireEntry;
    private static volatile long mappingEntry;
    private static volatile long exceptionEntry;
    private static volatile long objectRootEntry;
    private static volatile long loaderDrainEntry;
    private static volatile long typeArgsPrepareEntry;
    private static volatile long typeArgsProbeEntry;
    private static volatile long p6InstallEntry;
    private static volatile long p6ProbeEntry;
    private static volatile long closureInstallEntry;
    private static volatile long closureProbeEntry;
    private static volatile long ordinaryInstallEntry;
    private static volatile long ordinaryMarkSharedEntry;
    private static volatile long ordinaryProbeEntry;

    private ExternalModuleProbe() {}

    public static int callbackAbiVersion() {
        return CALLBACK_ABI_VERSION;
    }

    public static int value() {
        return 7;
    }

    public static void registerCallbacks(long bootstrap, long counters, long retire, long mapping,
                                         long exception, long objectRoot, long loaderDrain,
                                         long typeArgsPrepare, long typeArgsProbe,
                                         long p6Install, long p6Probe,
                                         long closureInstall, long closureProbe,
                                         long ordinaryInstall, long ordinaryMarkShared,
                                         long ordinaryProbe) {
        bootstrapEntry = bootstrap;
        countersEntry = counters;
        retireEntry = retire;
        mappingEntry = mapping;
        exceptionEntry = exception;
        objectRootEntry = objectRoot;
        loaderDrainEntry = loaderDrain;
        typeArgsPrepareEntry = typeArgsPrepare;
        typeArgsProbeEntry = typeArgsProbe;
        p6InstallEntry = p6Install;
        p6ProbeEntry = p6Probe;
        closureInstallEntry = closureInstall;
        closureProbeEntry = closureProbe;
        ordinaryInstallEntry = ordinaryInstall;
        ordinaryMarkSharedEntry = ordinaryMarkShared;
        ordinaryProbeEntry = ordinaryProbe;
    }

    public static long bootstrapEntry() {
        return bootstrapEntry;
    }

    public static long countersEntry() {
        return countersEntry;
    }

    public static long retireEntry() {
        return retireEntry;
    }

    public static long mappingEntry() {
        return mappingEntry;
    }

    public static long exceptionEntry() {
        return exceptionEntry;
    }

    public static long objectRootEntry() {
        return objectRootEntry;
    }

    public static long loaderDrainEntry() {
        return loaderDrainEntry;
    }

    public static long typeArgsPrepareEntry() {
        return typeArgsPrepareEntry;
    }

    public static long typeArgsProbeEntry() {
        return typeArgsProbeEntry;
    }

    public static long p6InstallEntry() {
        return p6InstallEntry;
    }

    public static long p6ProbeEntry() {
        return p6ProbeEntry;
    }

    public static long closureInstallEntry() {
        return closureInstallEntry;
    }

    public static long closureProbeEntry() {
        return closureProbeEntry;
    }

    public static long ordinaryInstallEntry() {
        return ordinaryInstallEntry;
    }

    public static long ordinaryMarkSharedEntry() {
        return ordinaryMarkSharedEntry;
    }

    public static long ordinaryProbeEntry() {
        return ordinaryProbeEntry;
    }
}
