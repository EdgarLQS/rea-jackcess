import io.github.spannm.jackcess.Column;
import io.github.spannm.jackcess.DataType;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.nio.file.Paths;
import java.util.Arrays;
import java.util.Iterator;
import java.util.List;

/** Verifies C++ TEXT and BINARY row writes with Jackcess 5.2.0. */
public final class JackcessVariableRowWriteCheck {
    private JackcessVariableRowWriteCheck() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 1) {
            throw new IllegalArgumentException("Usage: JackcessVariableRowWriteCheck <database.mdb>");
        }

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).withReadOnly(true).open()) {
            Table table = database.getTable("Variable");
            if (table == null || table.getColumns().size() != 4 || table.getRowCount() != 4) {
                throw new AssertionError("Jackcess could not find the expected four-row variable table");
            }
            List<? extends Column> columns = table.getColumns();
            DataType[] expectedTypes = {DataType.LONG, DataType.TEXT, DataType.BINARY, DataType.TEXT};
            String[] expectedNames = {"Id", "Name", "Payload", "Note"};
            for (int index = 0; index < expectedTypes.length; ++index) {
                if (!expectedNames[index].equals(columns.get(index).getName()) ||
                    columns.get(index).getType() != expectedTypes[index]) {
                    throw new AssertionError("Jackcess observed the wrong variable column schema");
                }
            }

            Iterator<Row> rows = table.iterator();
            Row updated = rows.next();
            if (!Integer.valueOf(10).equals(updated.get("Id")) ||
                !"B".equals(updated.get("Name")) ||
                !Arrays.equals(new byte[] {1, 2, 3}, (byte[]) updated.get("Payload")) ||
                !"🚀".equals(updated.get("Note"))) {
                throw new AssertionError("Jackcess did not observe the same-size TEXT/BINARY row update");
            }

            Row empty = rows.next();
            if (!Integer.valueOf(2).equals(empty.get("Id")) ||
                !"".equals(empty.get("Name")) ||
                !Arrays.equals(new byte[0], (byte[]) empty.get("Payload")) ||
                empty.get("Note") != null) {
                throw new AssertionError("Jackcess confused empty variable values with NULL");
            }

            Row nullable = rows.next();
            if (!Integer.valueOf(3).equals(nullable.get("Id")) ||
                nullable.get("Name") != null ||
                !Arrays.equals(new byte[] {(byte) 0x80}, (byte[]) nullable.get("Payload")) ||
                !"雪".equals(nullable.get("Note"))) {
                throw new AssertionError("Jackcess read variable column offsets or NULL values incorrectly");
            }

            Row boundary = rows.next();
            byte[] maxBinary = new byte[255];
            Arrays.fill(maxBinary, (byte) 0xa5);
            if (!Integer.valueOf(4).equals(boundary.get("Id")) ||
                !"x".repeat(255).equals(boundary.get("Name")) ||
                !Arrays.equals(maxBinary, (byte[]) boundary.get("Payload")) ||
                !"y".repeat(255).equals(boundary.get("Note")) || rows.hasNext()) {
                throw new AssertionError("Jackcess did not observe maximum TEXT/BINARY values");
            }
        }

        System.out.println("Jackcess 5.2.0 verified TEXT/BINARY types, Unicode, empty values, NULL, and update");
    }
}
