import io.github.spannm.jackcess.Cursor;
import io.github.spannm.jackcess.CursorBuilder;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.math.BigDecimal;
import java.nio.file.Paths;

/**
 * Verifies that Jackcess 5.2.0 reads ascending MONEY (Currency) index keys written by C++.
 *
 * The C++ fixture inserts 100, -0.0001, -2341234.0345, 0 and NULL, moves 100 to -5.5 and
 * deletes the NULL row, so the index must expose -2341234.0345, -5.5, -0.0001 and 0.
 */
public final class JackcessMoneyIndexCheck {
    private JackcessMoneyIndexCheck() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 3) {
            throw new IllegalArgumentException(
                "Usage: JackcessMoneyIndexCheck <database.mdb> <table> <index>");
        }

        String[] expectedAmounts = {"-2341234.0345", "-5.5", "-0.0001", "0"};
        int[] expectedIds = {3, 1, 2, 4};

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).withReadOnly(true).open()) {
            Table table = database.getTable(args[1]);
            if (table == null || table.getRowCount() != 4) {
                throw new AssertionError("Jackcess did not find the expected four-row table");
            }
            Cursor cursor = new CursorBuilder(table).withIndexByName(args[2]).toCursor();
            for (int index = 0; index < expectedAmounts.length; ++index) {
                Row row = cursor.getNextRow();
                if (row == null) {
                    throw new AssertionError("Jackcess read too few MONEY index entries");
                }
                BigDecimal amount = (BigDecimal) row.get("Amount");
                int id = ((Number) row.get("Id")).intValue();
                if (amount == null || amount.compareTo(new BigDecimal(expectedAmounts[index])) != 0
                        || id != expectedIds[index]) {
                    throw new AssertionError("Jackcess read " + amount + "/" + id
                        + " instead of " + expectedAmounts[index] + "/" + expectedIds[index]);
                }
            }
            if (cursor.getNextRow() != null) {
                throw new AssertionError("Jackcess observed an unexpected extra MONEY index entry");
            }
        }

        System.out.println("Jackcess 5.2.0 read the MONEY index entries in ascending amount order");
    }
}
