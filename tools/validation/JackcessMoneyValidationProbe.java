import io.github.spannm.jackcess.ColumnBuilder;
import io.github.spannm.jackcess.DataType;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.PropertyMap;
import io.github.spannm.jackcess.Table;
import io.github.spannm.jackcess.TableBuilder;

import java.math.BigDecimal;
import java.nio.file.Path;

/** Confirms exact MONEY validation comparisons against the fixed Jackcess 5.2.0 baseline. */
public final class JackcessMoneyValidationProbe {
    private static void expectRejected(Table table, String rangeValue, String thresholdValue, String message)
            throws Exception {
        try {
            table.addRow(new BigDecimal(rangeValue), new BigDecimal(thresholdValue));
        } catch (Exception error) {
            if (error.getMessage() != null && error.getMessage().contains(message)) {
                return;
            }
            throw error;
        }
        throw new AssertionError("Jackcess accepted MONEY values " + rangeValue + "/" + thresholdValue);
    }

    public static void main(String[] args) throws Exception {
        Path path = Path.of(args[0]);
        Database.FileFormat format = Database.FileFormat.valueOf(args[1]);
        try (Database database = new DatabaseBuilder().withPath(path).withFileFormat(format).create()) {
            database.setEvaluateExpressions(true);
            Table table = new TableBuilder("MoneyRules")
                .addColumn(new ColumnBuilder("RangeValue", DataType.MONEY)
                    .withProperty(PropertyMap.VALIDATION_RULE_PROP, "between -1.2500 and +1.5e1")
                    .withProperty(PropertyMap.VALIDATION_TEXT_PROP, "MONEY value is out of range"))
                .addColumn(new ColumnBuilder("ThresholdValue", DataType.MONEY)
                    .withProperty(PropertyMap.VALIDATION_RULE_PROP, ">= 1e-5")
                    .withProperty(PropertyMap.VALIDATION_TEXT_PROP, "MONEY value is below threshold"))
                .putProperty(PropertyMap.VALIDATION_RULE_PROP,
                    "[RangeValue] >= -2.0 AND [ThresholdValue] < 2e0")
                .putProperty(PropertyMap.VALIDATION_TEXT_PROP, "MONEY table thresholds failed")
                .toTable(database);

            table.addRow(new BigDecimal("-1.25"), new BigDecimal("0.0001"));
            table.addRow(new BigDecimal("15.0000"), new BigDecimal("1.9999"));
            expectRejected(table, "-1.2501", "0.0001", "MONEY value is out of range");
            expectRejected(table, "0", "0", "MONEY value is below threshold");
            expectRejected(table, "0", "2.1", "MONEY table thresholds failed");
        }
        System.out.println("Jackcess 5.2.0 MONEY validation passed for " + format);
    }
}
