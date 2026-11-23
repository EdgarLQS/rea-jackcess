import io.github.spannm.jackcess.ColumnBuilder;
import io.github.spannm.jackcess.DataType;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.PropertyMap;
import io.github.spannm.jackcess.Table;
import io.github.spannm.jackcess.TableBuilder;

import java.nio.file.Path;

/** Confirms numeric table-validation comparisons against the fixed Jackcess 5.2.0 baseline. */
public final class JackcessTableNumericValidationProbe {
    private static void expectRejected(Table table, float single, double value) throws Exception {
        try {
            table.addRow(single, value);
        } catch (Exception error) {
            if (error.getMessage() != null && error.getMessage().contains("Numeric thresholds failed")) {
                return;
            }
            throw error;
        }
        throw new AssertionError("Jackcess accepted a row outside the floating-point table rule");
    }

    public static void main(String[] args) throws Exception {
        Path path = Path.of(args[0]);
        Database.FileFormat format = Database.FileFormat.valueOf(args[1]);
        try (Database database = new DatabaseBuilder().withPath(path).withFileFormat(format).create()) {
            database.setEvaluateExpressions(true);
            Table table = new TableBuilder("FloatRules")
                .addColumn(new ColumnBuilder("SingleValue", DataType.FLOAT))
                .addColumn(new ColumnBuilder("DoubleValue", DataType.DOUBLE))
                .putProperty(PropertyMap.VALIDATION_RULE_PROP,
                    "[SingleValue] >= -1.25 AND [DoubleValue] < 2.5e1")
                .putProperty(PropertyMap.VALIDATION_TEXT_PROP, "Numeric thresholds failed")
                .toTable(database);

            table.addRow(-1.0F, 24.5D);
            table.addRow(-1.25F, 24.99D);
            table.addRow(0.0F, 24.99D);
            expectRejected(table, -1.5F, 1.0D);
            expectRejected(table, 0.0F, 25.0D);
        }
        System.out.println("Jackcess 5.2.0 accepted and rejected the expected numeric rows for " + format);
    }
}
