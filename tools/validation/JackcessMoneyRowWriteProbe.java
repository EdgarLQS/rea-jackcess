import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.math.BigDecimal;
import java.nio.file.Paths;
import java.util.Iterator;

/** Writes and updates Jet4 MONEY rows with the pinned Jackcess 5.2.0 reference. */
public final class JackcessMoneyRowWriteProbe {
    private JackcessMoneyRowWriteProbe() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 1) {
            throw new IllegalArgumentException("Usage: JackcessMoneyRowWriteProbe <empty-money-database.mdb>");
        }

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).open()) {
            Table table = database.getTable("MoneyValues");
            if (table == null || table.iterator().hasNext()) {
                throw new AssertionError("Expected an empty C++-created MoneyValues table");
            }

            table.addRow(new BigDecimal("-2341234.03450"));
            table.addRow(new BigDecimal("37"));
            table.addRow(new BigDecimal("10000.45"));
            table.addRow(new BigDecimal("1E+2"));
            table.addRow(new BigDecimal("0E+1000001"));
            table.addRow(new BigDecimal("922337203685477.5807"));
            table.addRow(new BigDecimal("-922337203685477.5808"));

            Iterator<Row> rows = table.iterator();
            rows.next();
            rows.next();
            Row updated = rows.next();
            updated.put("Amount", new BigDecimal("-0.0001"));
            table.updateRow(updated);
        }

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).withReadOnly(true).open()) {
            Table table = database.getTable("MoneyValues");
            String[] expected = {
                "-2341234.0345",
                "37.0000",
                "-0.0001",
                "100.0000",
                "0.0000",
                "922337203685477.5807",
                "-922337203685477.5808"
            };
            if (table == null || table.getRowCount() != expected.length) {
                throw new AssertionError("Jackcess could not reopen the expected MONEY rows");
            }

            Iterator<Row> rows = table.iterator();
            for (String value : expected) {
                if (!rows.hasNext() || !new BigDecimal(value).equals(rows.next().get("Amount"))) {
                    throw new AssertionError("Jackcess did not preserve exact MONEY value " + value);
                }
            }
            if (rows.hasNext()) {
                throw new AssertionError("Jackcess observed an unexpected extra MONEY row");
            }
        }

        System.out.println("Jackcess 5.2.0 wrote, updated, and reopened exact MONEY values");
    }
}
