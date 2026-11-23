import io.github.spannm.jackcess.CursorBuilder;
import io.github.spannm.jackcess.DatabaseBuilder;
import java.nio.file.Path;
import java.util.Map;

public final class JackcessBinaryMatcherCheck {
    public static void main(String[] args) throws Exception {
        try (var db = new DatabaseBuilder().withPath(Path.of(args[0])).withReadOnly(true).open()) {
            var cursor = CursorBuilder.createCursor(db.getTable("BinaryValues"));
            if (!cursor.findFirstRow(Map.of("Payload", "hello")) ||
                !Integer.valueOf(1).equals(cursor.getCurrentRow().get("Id"))) {
                throw new AssertionError("serialized ASCII String did not match the BINARY value");
            }
            if (!cursor.findFirstRow(Map.of("Payload", "A\0\uD83D\uDE00")) ||
                !Integer.valueOf(2).equals(cursor.getCurrentRow().get("Id"))) {
                throw new AssertionError("serialized modified UTF-8 String did not match the BINARY value");
            }
            if (!cursor.findFirstRow(Map.of("Payload", new byte[] {0, (byte) 0xff, 0x10})) ||
                !Integer.valueOf(3).equals(cursor.getCurrentRow().get("Id"))) {
                throw new AssertionError("raw byte[] matching did not preserve byte equality");
            }
            System.out.println("Jackcess 5.2.0 matched serialized Strings and raw bytes in BINARY");
        }
    }
}
