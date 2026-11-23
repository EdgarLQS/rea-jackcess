import io.github.spannm.jackcess.ColumnBuilder;
import io.github.spannm.jackcess.DataType;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.PropertyMap;
import io.github.spannm.jackcess.Table;
import io.github.spannm.jackcess.TableBuilder;

import java.nio.file.Path;

/** Confirms numeric column-validation comparisons against the fixed Jackcess 5.2.0 baseline. */
public final class JackcessColumnNumericValidationProbe {
    private static void expectRejected(Table table, float single, double value, String message) throws Exception {
        try {
            table.addRow(single, value);
        } catch (Exception error) {
            if (error.getMessage() != null && error.getMessage().contains(message)) {
                return;
            }
            throw error;
        }
        throw new AssertionError("Jackcess accepted a value outside the numeric column rule");
    }

    public static void main(String[] args) throws Exception {
        Path path = Path.of(args[0]);
        Database.FileFormat format = Database.FileFormat.valueOf(args[1]);
        try (Database database = new DatabaseBuilder().withPath(path).withFileFormat(format).create()) {
            database.setEvaluateExpressions(true);
            Table table = new TableBuilder("NumericColumns")
                .addColumn(new ColumnBuilder("SingleValue", DataType.FLOAT)
                    .withProperty(PropertyMap.VALIDATION_RULE_PROP, "between -1.25 and +1.5e1")
                    .withProperty(PropertyMap.VALIDATION_TEXT_PROP, "SINGLE value is out of range"))
                .addColumn(new ColumnBuilder("DoubleValue", DataType.DOUBLE)
                    .withProperty(PropertyMap.VALIDATION_RULE_PROP, ">= -2.5e1")
                    .withProperty(PropertyMap.VALIDATION_TEXT_PROP, "DOUBLE value is too small"))
                .toTable(database);

            table.addRow(-1.25F, -25.0D);
            table.addRow(15.0F, 0.0D);
            expectRejected(table, -1.5F, 0.0D, "SINGLE value is out of range");
            expectRejected(table, 15.5F, 0.0D, "SINGLE value is out of range");
            expectRejected(table, 0.0F, -25.01D, "DOUBLE value is too small");
        }
        System.out.println("Jackcess 5.2.0 accepted and rejected the expected column values for " + format);
    }
}
