import io.github.spannm.jackcess.ColumnBuilder;
import io.github.spannm.jackcess.DataType;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.PropertyMap;
import io.github.spannm.jackcess.Table;
import io.github.spannm.jackcess.TableBuilder;

import java.math.BigDecimal;
import java.nio.file.Path;

/** Confirms numeric table BETWEEN behavior against the fixed Jackcess 5.2.0 baseline. */
public final class JackcessTableBetweenProbe {
    private static Table table(Database database, String name, DataType type, String rule) throws Exception {
        return new TableBuilder(name)
            .addColumn(new ColumnBuilder("Value", type))
            .putProperty(PropertyMap.VALIDATION_RULE_PROP, rule)
            .putProperty(PropertyMap.VALIDATION_TEXT_PROP, "Numeric BETWEEN failed")
            .toTable(database);
    }

    private static void expectRejected(Table table, Object value) throws Exception {
        try {
            table.addRow(value);
        } catch (Exception error) {
            if (error.getMessage() != null && error.getMessage().contains("Numeric BETWEEN failed")) {
                return;
            }
            throw error;
        }
        throw new AssertionError("Jackcess accepted an out-of-range value in " + table.getName());
    }

    private static void expectColumnRejected(Table table, Object value) throws Exception {
        try {
            table.addRow(value);
        } catch (Exception error) {
            if (error.getMessage() != null && error.getMessage().contains("Column BETWEEN failed")) {
                return;
            }
            throw error;
        }
        throw new AssertionError("Jackcess accepted an out-of-range column value");
    }

    public static void main(String[] args) throws Exception {
        Path path = Path.of(args[0]);
        Database.FileFormat format = Database.FileFormat.valueOf(args[1]);
        try (Database database = new DatabaseBuilder().withPath(path).withFileFormat(format).create()) {
            database.setEvaluateExpressions(true);

            Table integerTable = table(database, "IntegerBetween", DataType.LONG,
                "[Value] BETWEEN +2 AND -2");
            integerTable.addRow(-2);
            integerTable.addRow(2);
            expectRejected(integerTable, -3);
            expectRejected(integerTable, 3);

            Table floatingTable = table(database, "FloatingBetween", DataType.DOUBLE,
                "[Value] BETWEEN 1e-3 AND -1e-3");
            floatingTable.addRow(-0.001D);
            floatingTable.addRow(0.001D);
            expectRejected(floatingTable, -0.0011D);
            expectRejected(floatingTable, 0.0011D);

            Table moneyTable = table(database, "MoneyBetween", DataType.MONEY,
                "[Value] BETWEEN 1.25 AND -1.25");
            moneyTable.addRow(new BigDecimal("-1.25"));
            moneyTable.addRow(new BigDecimal("1.25"));
            expectRejected(moneyTable, new BigDecimal("-1.2501"));
            expectRejected(moneyTable, new BigDecimal("1.2501"));

            Table columnTable = new TableBuilder("ColumnBetween")
                .addColumn(new ColumnBuilder("Value", DataType.LONG)
                    .withProperty(PropertyMap.VALIDATION_RULE_PROP, "between 10 and 7")
                    .withProperty(PropertyMap.VALIDATION_TEXT_PROP, "Column BETWEEN failed"))
                .toTable(database);
            columnTable.addRow(7);
            columnTable.addRow(10);
            expectColumnRejected(columnTable, 6);
            expectColumnRejected(columnTable, 11);

            Table floatingColumnTable = new TableBuilder("FloatingColumnBetween")
                .addColumn(new ColumnBuilder("Value", DataType.DOUBLE)
                    .withProperty(PropertyMap.VALIDATION_RULE_PROP, "between 1e-3 and -1e-3")
                    .withProperty(PropertyMap.VALIDATION_TEXT_PROP, "Column BETWEEN failed"))
                .toTable(database);
            floatingColumnTable.addRow(-0.001D);
            floatingColumnTable.addRow(0.001D);
            expectColumnRejected(floatingColumnTable, -0.0011D);
            expectColumnRejected(floatingColumnTable, 0.0011D);

            Table moneyColumnTable = new TableBuilder("MoneyColumnBetween")
                .addColumn(new ColumnBuilder("Value", DataType.MONEY)
                    .withProperty(PropertyMap.VALIDATION_RULE_PROP, "between 1.25 and -1.25")
                    .withProperty(PropertyMap.VALIDATION_TEXT_PROP, "Column BETWEEN failed"))
                .toTable(database);
            moneyColumnTable.addRow(new BigDecimal("-1.25"));
            moneyColumnTable.addRow(new BigDecimal("1.25"));
            expectColumnRejected(moneyColumnTable, new BigDecimal("-1.2501"));
            expectColumnRejected(moneyColumnTable, new BigDecimal("1.2501"));
        }
        System.out.println("Jackcess 5.2.0 numeric BETWEEN passed for " + format);
    }
}
