import io.github.spannm.jackcess.Column;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Table;

import java.io.File;
import java.math.BigDecimal;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.time.LocalDateTime;
import java.util.Map;
import java.util.TreeSet;
import java.util.UUID;

/**
 * Dumps every user and system table of a database as JSON so that the C++ canonical output can be
 * compared cell by cell with the fixed Jackcess 5.2.0 JAR.
 *
 * <p>Used with {@code compare_table_dump.py}; see tests/fixtures/README.md for the commands. The
 * dump keeps BLOB payloads as length plus SHA-256 and prints numbers, decimals, dates, and GUIDs
 * so the comparer can normalise formatting differences.
 */
public final class JackcessTableDump {

    private static String quote(String value) {
        StringBuilder builder = new StringBuilder("\"");
        for (char character : value.toCharArray()) {
            switch (character) {
                case '"': builder.append("\\\""); break;
                case '\\': builder.append("\\\\"); break;
                case '\n': builder.append("\\n"); break;
                case '\r': builder.append("\\r"); break;
                case '\t': builder.append("\\t"); break;
                default:
                    if (character < 0x20) {
                        builder.append(String.format("\\u%04x", (int) character));
                    } else {
                        builder.append(character);
                    }
            }
        }
        return builder.append('"').toString();
    }

    private static String hex(byte[] bytes) {
        StringBuilder builder = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) {
            builder.append(String.format("%02x", value));
        }
        return builder.toString();
    }

    private static String value(Object value) throws Exception {
        if (value == null) {
            return "null";
        }
        if (value instanceof Boolean) {
            return ((Boolean) value) ? "true" : "false";
        }
        if (value instanceof Byte || value instanceof Short || value instanceof Integer || value instanceof Long) {
            return value.toString();
        }
        if (value instanceof Float || value instanceof Double) {
            if (value instanceof Double && ((Double) value).isNaN()
                    || value instanceof Float && ((Float) value).isNaN()) {
                return "{\"$float\":\"NaN\"}";
            }
            return value.toString();
        }
        if (value instanceof BigDecimal) {
            return "{\"$decimal\":" + quote(((BigDecimal) value).toPlainString()) + "}";
        }
        if (value instanceof LocalDateTime) {
            return "{\"$date\":" + quote(value.toString()) + "}";
        }
        if (value instanceof UUID) {
            return "{\"$guid\":" + quote("{" + value + "}") + "}";
        }
        if (value instanceof byte[]) {
            byte[] bytes = (byte[]) value;
            return "{\"$blob\":{\"size\":" + bytes.length + ",\"sha256\":"
                + quote(hex(MessageDigest.getInstance("SHA-256").digest(bytes))) + "}}";
        }
        if (value instanceof String) {
            return quote((String) value);
        }
        return quote(value.toString());
    }

    private static void dumpTable(StringBuilder output, Table table, boolean user) throws Exception {
        output.append("{\"name\":").append(quote(table.getName()))
            .append(",\"isUserTable\":").append(user)
            .append(",\"columns\":[");
        boolean first = true;
        for (Column column : table.getColumns()) {
            if (!first) {
                output.append(',');
            }
            first = false;
            output.append("{\"name\":").append(quote(column.getName()))
                .append(",\"type\":").append(quote(column.getType().name()))
                .append(",\"length\":").append(column.getLength()).append('}');
        }
        output.append("],\"rows\":[");
        boolean firstRow = true;
        for (Map<String, Object> row : table) {
            if (!firstRow) {
                output.append(',');
            }
            firstRow = false;
            output.append('[');
            boolean firstValue = true;
            for (Column column : table.getColumns()) {
                if (!firstValue) {
                    output.append(',');
                }
                firstValue = false;
                output.append(value(row.get(column.getName())));
            }
            output.append(']');
        }
        output.append("]}");
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 2 && args.length != 3) {
            System.err.println("usage: JackcessTableDump <database> <output.json> [charset-name]");
            System.exit(2);
        }
        StringBuilder output = new StringBuilder("{\"tables\":[");
        boolean first = true;
        try (Database db = DatabaseBuilder.open(new File(args[0]))) {
            if (args.length == 3) {
                // Mirrors a caller overriding the Jet3 legacy-text charset on the fixed Jackcess.
                db.setCharset(java.nio.charset.Charset.forName(args[2]));
            }
            for (String name : db.getTableNames()) {
                if (!first) {
                    output.append(',');
                }
                first = false;
                dumpTable(output, db.getTable(name), true);
            }
            for (String name : new TreeSet<>(db.getSystemTableNames())) {
                // Temporary query tables carry a generated f_ prefix and are not part of the file.
                if (name.startsWith("f_")) {
                    continue;
                }
                if (!first) {
                    output.append(',');
                }
                first = false;
                dumpTable(output, db.getSystemTable(name), false);
            }
        }
        output.append("]}");
        Files.writeString(new File(args[1]).toPath(), output.toString(), StandardCharsets.UTF_8);
        System.out.println("Dumped " + args[0] + " to " + args[1]);
    }
}
