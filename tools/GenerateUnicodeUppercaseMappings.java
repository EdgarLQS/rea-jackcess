import java.io.BufferedWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Locale;

/** Generates full Unicode uppercase expansions used by Java Locale.ROOT name keys. */
public final class GenerateUnicodeUppercaseMappings {
    private static final int MAX_CODE_POINTS = 3;

    private GenerateUnicodeUppercaseMappings() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 1) {
            throw new IllegalArgumentException(
                    "usage: GenerateUnicodeUppercaseMappings <output.inc>");
        }

        long mappingCount = 0;
        try (BufferedWriter writer =
                     Files.newBufferedWriter(Path.of(args[0]), StandardCharsets.UTF_8)) {
            writer.write("// Generated with OpenJDK " + Runtime.version() + ". Regenerate with:\n");
            writer.write("// java tools/GenerateUnicodeUppercaseMappings.java ");
            writer.write("src/value/unicode_uppercase_mappings.inc\n");
            for (int codePoint = Character.MIN_CODE_POINT;
                 codePoint <= Character.MAX_CODE_POINT;
                 codePoint++) {
                if (codePoint >= Character.MIN_SURROGATE &&
                    codePoint <= Character.MAX_SURROGATE) {
                    continue;
                }

                int[] fullUppercase = new String(Character.toChars(codePoint))
                        .toUpperCase(Locale.ROOT)
                        .codePoints()
                        .toArray();
                int simpleUppercase = Character.toUpperCase(codePoint);
                if (fullUppercase.length == 1 && fullUppercase[0] == simpleUppercase) {
                    continue;
                }
                if (fullUppercase.length == 0 || fullUppercase.length > MAX_CODE_POINTS) {
                    throw new IllegalStateException(
                            "Unexpected uppercase mapping length for U+" +
                                    Integer.toHexString(codePoint));
                }

                writer.write(String.format(Locale.ROOT,
                        "{0x%06xu, {0x%06xu, 0x%06xu, 0x%06xu}, %du},%n",
                        codePoint,
                        fullUppercase.length > 0 ? fullUppercase[0] : 0,
                        fullUppercase.length > 1 ? fullUppercase[1] : 0,
                        fullUppercase.length > 2 ? fullUppercase[2] : 0,
                        fullUppercase.length));
                mappingCount++;
            }
        }
        System.out.println("Wrote " + mappingCount + " full uppercase mappings using OpenJDK " +
                Runtime.version());
    }
}
