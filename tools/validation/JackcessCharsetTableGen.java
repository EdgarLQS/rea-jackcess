import java.nio.ByteBuffer;
import java.nio.CharBuffer;
import java.nio.charset.Charset;
import java.nio.charset.CharsetDecoder;
import java.nio.charset.CodingErrorAction;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;

/**
 * Generates the Jet3 code-page decode tables used by src/util/code_page.cpp.
 *
 * <p>Jackcess decodes legacy text with a Java {@link Charset} (Jet3 callers may pick any charset),
 * so Java NIO is the authority for how every byte sequence decodes. The generator probes the
 * charset with {@link CodingErrorAction#REPLACE} and records the decoded code point of every byte
 * and every (lead, trail) pair; the C++ decoder replays those results, so malformed sequences
 * behave exactly like Jackcess: one U+FFFD per malformed run.
 *
 * <p>The probe results have a uniform shape that the generator verifies before emitting:
 * <ul>
 *   <li>a single byte always decodes to exactly one BMP code point (U+FFFD when malformed),
 *   <li>a lead byte followed by a trail outside the table window decodes to U+FFFD plus the
 *       trail's own single-byte decode, consuming both bytes,
 *   <li>a lead byte followed by 0xFF decodes to a lone U+FFFD, consuming both bytes,
 *   <li>every table-window pair decodes to one BMP code point, a lone U+FFFD, or U+FFFD plus the
 *       trail's single-byte decode.
 * </ul>
 *
 * <p>Usage: JackcessCharsetTableGen &lt;charset-name&gt; &lt;code-page&gt; &lt;output.inc&gt;
 */
public final class JackcessCharsetTableGen {

    /** Sentinel: emit a lone U+FFFD and consume the pair. */
    static final int REPLACE_ONLY = 0xFFFD;
    /** Sentinel: emit U+FFFD plus the trail byte's own single-byte decode, consume the pair. */
    static final int REPLACE_THEN_TRAIL = 0xFFFE;
    static final int REPLACEMENT = 0xFFFD;
    static final int TRAIL_MIN = 0x40;
    static final int TRAIL_MAX = 0xFE;

    private JackcessCharsetTableGen() {
    }

    private record Probe(String text, int consumed) {
    }

    private static Probe probe(Charset charset, byte[] bytes) {
        CharsetDecoder decoder = charset.newDecoder()
            .onMalformedInput(CodingErrorAction.REPLACE)
            .onUnmappableCharacter(CodingErrorAction.REPLACE);
        ByteBuffer in = ByteBuffer.wrap(bytes);
        CharBuffer out = CharBuffer.allocate(bytes.length * 2 + 4);
        decoder.decode(in, out, true);
        return new Probe(out.flip().toString(), bytes.length - in.remaining());
    }

    private static int singleCodePoint(Probe probe) {
        if (probe.text().codePointCount(0, probe.text().length()) != 1) {
            throw new IllegalStateException("single byte decoded to " + probe.text().length()
                + " chars: " + describe(probe));
        }
        return probe.text().codePointAt(0);
    }

    private static String describe(Probe probe) {
        StringBuilder builder = new StringBuilder();
        probe.text().codePoints().forEach(cp -> builder.append(String.format("U+%04X ", cp)));
        return builder.toString().trim() + " (consumed " + probe.consumed() + ")";
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 3) {
            System.err.println("usage: JackcessCharsetTableGen <charset-name> <code-page> <output.inc>");
            System.exit(2);
        }
        final String charsetName = args[0];
        final int codePage = Integer.parseInt(args[1]);
        final Path output = Paths.get(args[2]);
        final Charset charset = Charset.forName(charsetName);

        final int[] single = new int[256];
        for (int b = 0; b < 256; ++b) {
            single[b] = singleCodePoint(probe(charset, new byte[] {(byte) b}));
        }

        // A byte is a DBCS lead when decoding it alone is malformed.
        final boolean[] isLead = new boolean[256];
        int leadCount = 0;
        for (int b = 0x80; b < 256; ++b) {
            CharsetDecoder decoder = charset.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT);
            isLead[b] = decoder.decode(ByteBuffer.wrap(new byte[] {(byte) b}),
                                       CharBuffer.allocate(4), true).isMalformed();
            if (isLead[b]) {
                ++leadCount;
            }
        }

        final int rows = leadCount == 0 ? 1 : leadCount;
        final int[][] table = new int[rows][TRAIL_MAX - TRAIL_MIN + 1];
        final int[] rowOf = new int[256];
        java.util.Arrays.fill(rowOf, -1);
        int row = 0;
        for (int lead = 0x80; lead < 256; ++lead) {
            if (!isLead[lead]) {
                continue;
            }
            rowOf[lead] = row;
            for (int trail = 0; trail < 256; ++trail) {
                final Probe pair = probe(charset, new byte[] {(byte) lead, (byte) trail});
                final int[] expected;
                if (pair.consumed() != 2) {
                    throw new IllegalStateException(String.format(
                        "pair %02X %02X consumed %d bytes: %s", lead, trail, pair.consumed(),
                        describe(pair)));
                }
                if (trail < TRAIL_MIN || trail > TRAIL_MAX) {
                    // Outside the window the decoder replaces the lead and re-decodes the trail.
                    expected = new int[] {REPLACEMENT, single[trail]};
                } else if (trail == 0xFF) {
                    expected = new int[] {REPLACEMENT};
                } else {
                    final int chars = pair.text().codePointCount(0, pair.text().length());
                    if (chars == 1) {
                        expected = new int[] {pair.text().codePointAt(0)};
                    } else {
                        expected = new int[] {REPLACEMENT, single[trail]};
                    }
                }
                int encoded;
                if (expected.length == 1) {
                    encoded = expected[0];
                } else if (expected.length == 2 && expected[0] == REPLACEMENT) {
                    encoded = REPLACE_THEN_TRAIL;
                } else {
                    throw new IllegalStateException(String.format(
                        "pair %02X %02X has an unexpected shape: %s", lead, trail, describe(pair)));
                }
                if (trail >= TRAIL_MIN && trail <= TRAIL_MAX) {
                    table[rowOf[lead]][trail - TRAIL_MIN] = encoded;
                } else if (encoded != REPLACE_THEN_TRAIL && !(trail == 0xFF && encoded == REPLACE_ONLY)) {
                    throw new IllegalStateException(String.format(
                        "pair %02X %02X violates the uniform out-of-window rule: %s",
                        lead, trail, describe(pair)));
                }
            }
            ++row;
        }

        final StringBuilder out = new StringBuilder();
        final String prefix = "cp" + codePage + "_";
        out.append("// Generated by tools/validation/JackcessCharsetTableGen.java from Java charset \"")
           .append(charsetName).append("\" (code page ").append(codePage)
           .append("); do not edit.\n");
        out.append("// Values are BMP code points decoded at runtime. Sentinels: 0xFFFD emits a lone\n");
        out.append("// U+FFFD; 0xFFFE emits U+FFFD plus the trail byte's own single-byte decode.\n\n");
        out.append("namespace {\n\n");
        out.append("constexpr std::uint16_t ").append(prefix).append("single[256] = {\n");
        for (int b = 0; b < 256; b += 8) {
            out.append("    ");
            for (int k = 0; k < 8; ++k) {
                out.append(String.format("0x%04X, ", single[b + k]));
            }
            out.append('\n');
        }
        out.append("};\n");
        if (leadCount > 0) {
            out.append("\n// Rows are lead bytes 0x80..0xFF; columns are trails 0x")
               .append(Integer.toHexString(TRAIL_MIN)).append("..0x")
               .append(Integer.toHexString(TRAIL_MAX)).append(".\n");
            out.append("constexpr std::uint16_t ").append(prefix).append("double[").append(rows)
               .append("][").append(TRAIL_MAX - TRAIL_MIN + 1).append("] = {\n");
            for (int lead = 0x80; lead < 256; ++lead) {
                if (!isLead[lead]) {
                    continue;
                }
                out.append("    {\n");
                for (int trail = TRAIL_MIN; trail <= TRAIL_MAX; trail += 8) {
                    out.append("        ");
                    for (int k = 0; k < 8 && trail + k <= TRAIL_MAX; ++k) {
                        out.append(String.format("0x%04X, ", table[rowOf[lead]][trail + k - TRAIL_MIN]));
                    }
                    out.append('\n');
                }
                out.append("    },\n");
            }
            out.append("};\n");
            out.append("\n// Maps a byte to its row in ").append(prefix).append("double, or 255 when the byte is not a lead.\n");
            out.append("constexpr std::uint8_t ").append(prefix).append("lead_row[128] = {\n    ");
            for (int b = 0x80; b < 256; ++b) {
                out.append(rowOf[b] < 0 ? "255, " : String.format("%d, ", rowOf[b]));
                if ((b + 1) % 8 == 0 && b < 255) {
                    out.append('\n').append("    ");
                }
            }
            out.append("\n};\n");
        }
        out.append("\n}\n");
        Files.writeString(output, out.toString());
        System.out.println("Wrote " + output + " (charset " + charsetName + ", code page " + codePage
            + ", " + leadCount + " lead rows, " + rows * (TRAIL_MAX - TRAIL_MIN + 1) + " pairs)");
    }

}
