import io.github.spannm.jackcess.DataType;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.nio.file.Paths;
import java.util.Iterator;
import java.util.Locale;

/** Verifies C++ Jet4 GUID writes with Jackcess 5.2.0. */
public final class JackcessGuidWriteCheck {
    private JackcessGuidWriteCheck() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 1) {
            throw new IllegalArgumentException("Usage: JackcessGuidWriteCheck <database.mdb>");
        }

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).withReadOnly(true).open()) {
            Table table = database.getTable("Guids");
            if (table == null || table.getRowCount() != 2 || table.getColumns().size() != 2 ||
                !"Id".equals(table.getColumns().get(0).getName()) ||
                !"Uuid".equals(table.getColumns().get(1).getName()) ||
                table.getColumns().get(1).getType() != DataType.GUID) {
                throw new AssertionError("Jackcess did not observe the expected two-column GUID table");
            }

            Iterator<Row> rows = table.iterator();
            Row updated = rows.next();
            Row original = rows.next();
            if (!Integer.valueOf(1).equals(updated.get("Id")) ||
                !"fedcba98-7654-3210-fedc-ba9876543210".equals(normalize(updated.get("Uuid"))) ||
                !Integer.valueOf(2).equals(original.get("Id")) ||
                !"01234567-89ab-cdef-0123-456789abcdef".equals(normalize(original.get("Uuid"))) ||
                rows.hasNext()) {
                throw new AssertionError("Jackcess observed incorrect GUID values or ordering");
            }
        }

        System.out.println("Jackcess 5.2.0 verified GUID schema, byte order, insert, and update");
    }

    private static String normalize(Object value) {
        return String.valueOf(value).replace("{", "").replace("}", "").toLowerCase(Locale.ROOT);
    }
}
