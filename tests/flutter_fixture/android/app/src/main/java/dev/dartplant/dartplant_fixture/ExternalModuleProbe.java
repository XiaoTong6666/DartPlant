package dev.dartplant.dartplant_fixture;

/**
 * Deliberately tiny Java/Native rendezvous point for the external libxposed
 * integration tests. Production Dart AOT hooks use the independent C ABI.
 */
public final class ExternalModuleProbe {
    private static volatile long bootstrapEntry;
    private static volatile long countersEntry;
    private static volatile long retireEntry;
    private static volatile long mappingEntry;
    private static volatile long exceptionEntry;
    private static volatile long objectRootEntry;
    private static volatile long loaderDrainEntry;

    private ExternalModuleProbe() {}

    public static int value() {
        return 7;
    }

    public static void registerCallbacks(long bootstrap, long counters, long retire, long mapping,
                                         long exception, long objectRoot, long loaderDrain) {
        bootstrapEntry = bootstrap;
        countersEntry = counters;
        retireEntry = retire;
        mappingEntry = mapping;
        exceptionEntry = exception;
        objectRootEntry = objectRoot;
        loaderDrainEntry = loaderDrain;
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
}
