import io.github.spannm.jackcess.Column;
import io.github.spannm.jackcess.DataType;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.math.BigDecimal;
import java.nio.file.Paths;
import java.util.Iterator;

/** Verifies C++ Jet4 MONEY writes with Jackcess 5.2.0. */
public final class JackcessMoneyWriteCheck {
    private JackcessMoneyWriteCheck() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 1) {
            throw new IllegalArgumentException("Usage: JackcessMoneyWriteCheck <database.mdb>");
        }

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).withReadOnly(true).open()) {
            Table table = database.getTable("MoneyValues");
            if (table == null || table.getColumns().size() != 1 || table.getRowCount() != 7) {
                throw new AssertionError("Jackcess could not find the expected seven-row MONEY table");
            }
            Column amount = table.getColumns().get(0);
            if (!"Amount".equals(amount.getName()) || amount.getType() != DataType.MONEY) {
                throw new AssertionError("Jackcess observed the wrong MONEY schema");
            }

            String[] expected = {
                "-2341234.0345",
                "37.0000",
                "-0.0001",
                "100.0000",
                "0.0000",
                "922337203685477.5807",
                "-922337203685477.5808"
            };
            Iterator<Row> rows = table.iterator();
            for (String value : expected) {
                if (!rows.hasNext() || !new BigDecimal(value).equals(rows.next().get("Amount"))) {
                    throw new AssertionError("Jackcess did not observe exact MONEY value " + value);
                }
            }
            if (rows.hasNext()) {
                throw new AssertionError("Jackcess observed an unexpected extra MONEY row");
            }
        }

        System.out.println("Jackcess 5.2.0 verified exact MONEY writes, update, scale, and signed boundaries");
    }
}
