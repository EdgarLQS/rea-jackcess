import io.github.spannm.jackcess.Column;
import io.github.spannm.jackcess.DataType;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.nio.file.Paths;
import java.util.Iterator;

/** Verifies C++ Jet4 BOOLEAN writes with Jackcess 5.2.0. */
public final class JackcessBooleanWriteCheck {
    private JackcessBooleanWriteCheck() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 1) {
            throw new IllegalArgumentException("Usage: JackcessBooleanWriteCheck <database.mdb>");
        }

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).withReadOnly(true).open()) {
            Table table = database.getTable("Booleans");
            if (table == null || table.getColumns().size() != 4 || table.getRowCount() != 3) {
                throw new AssertionError("Jackcess could not find the expected three-row BOOLEAN table");
            }

            Column flag = table.getColumns().get(1);
            Column maybe = table.getColumns().get(2);
            if (!"Flag".equals(flag.getName()) || flag.getType() != DataType.BOOLEAN || flag.getLength() != 1
                    || flag.isVariableLength()) {
                throw new AssertionError("Jackcess observed the wrong first BOOLEAN column layout");
            }
            if (!"Maybe".equals(maybe.getName()) || maybe.getType() != DataType.BOOLEAN || maybe.getLength() != 1
                    || maybe.isVariableLength()) {
                throw new AssertionError("Jackcess observed the wrong second BOOLEAN column layout");
            }

            boolean[][] expected = {
                {false, false},
                {false, true},
                {false, false},
            };
            int[] ids = {1, 2, 3};
            int[] tails = {11, -5, 0};

            Iterator<Row> rows = table.iterator();
            for (int index = 0; index < expected.length; index++) {
                if (!rows.hasNext()) {
                    throw new AssertionError("Jackcess observed fewer BOOLEAN rows than expected");
                }
                Row row = rows.next();
                if (((Number) row.get("Id")).intValue() != ids[index]
                        || !Boolean.valueOf(expected[index][0]).equals(row.get("Flag"))
                        || !Boolean.valueOf(expected[index][1]).equals(row.get("Maybe"))
                        || ((Number) row.get("Tail")).intValue() != tails[index]) {
                    throw new AssertionError("Jackcess did not observe the expected BOOLEAN row " + index);
                }
            }
            if (rows.hasNext()) {
                throw new AssertionError("Jackcess observed an unexpected extra BOOLEAN row");
            }
        }

        System.out.println("Jackcess 5.2.0 verified BOOLEAN bit values, FALSE/TRUE updates, and the trailing LONG");
    }
}
