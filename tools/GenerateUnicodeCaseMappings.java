import java.io.BufferedWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

/** Generates the simple case mappings used to mirror Java String.equalsIgnoreCase. */
public final class GenerateUnicodeCaseMappings {
    private GenerateUnicodeCaseMappings() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 1) {
            throw new IllegalArgumentException("usage: GenerateUnicodeCaseMappings <output.inc>");
        }

        long mappingCount = 0;
        try (BufferedWriter writer = Files.newBufferedWriter(Path.of(args[0]), StandardCharsets.UTF_8)) {
            writer.write("// Generated with OpenJDK " + Runtime.version() + ". Regenerate with:\n");
            writer.write("// java tools/GenerateUnicodeCaseMappings.java src/value/unicode_case_mappings.inc\n");
            for (int codePoint = Character.MIN_CODE_POINT;
                 codePoint <= Character.MAX_CODE_POINT;
                 codePoint++) {
                if (codePoint >= Character.MIN_SURROGATE && codePoint <= Character.MAX_SURROGATE) {
                    continue;
                }
                int upper = Character.toUpperCase(codePoint);
                int lower = Character.toLowerCase(codePoint);
                if (upper == codePoint && lower == codePoint) {
                    continue;
                }
                writer.write(String.format("{0x%06xu, 0x%06xu, 0x%06xu},\n",
                        codePoint, upper, lower));
                mappingCount++;
            }
        }
        System.out.println("Wrote " + mappingCount + " mappings using OpenJDK " + Runtime.version());
    }
}
