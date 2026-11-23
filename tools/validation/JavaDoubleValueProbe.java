public final class JavaDoubleValueProbe {
    private static void expect(String text, double expected) {
        double actual = Double.valueOf(text);
        if (Double.compare(actual, expected) != 0) {
            throw new AssertionError(text + " parsed as " + actual + ", expected " + expected);
        }
    }

    private static void expectInvalid(String text) {
        try {
            Double.valueOf(text);
            throw new AssertionError(text + " unexpectedly parsed");
        } catch (NumberFormatException expected) {
            // Expected: SimpleColumnMatcher does not swallow this conversion failure.
        }
    }

    public static void main(String[] args) {
        expect("13.5d", 13.5);
        expect("13.5F", 13.5);
        expect("0x1.bp3", 13.5);
        expect("Infinity", Double.POSITIVE_INFINITY);
        expect("NaN", Double.NaN);
        expect(" \t13.5d\n", 13.5);
        expectInvalid("inf");
        expectInvalid("NaNd");
        System.out.println("Double.valueOf vectors passed on OpenJDK " + Runtime.version());
    }
}
