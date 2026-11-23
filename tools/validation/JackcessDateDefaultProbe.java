import io.github.spannm.jackcess.ColumnBuilder;
import io.github.spannm.jackcess.DataType;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.PropertyMap;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;
import io.github.spannm.jackcess.TableBuilder;

import java.nio.file.Path;
import java.time.LocalDateTime;

/** Confirms date-only SHORT_DATE_TIME defaults against the fixed Jackcess 5.2.0 baseline. */
public final class JackcessDateDefaultProbe {
    private static void expectRejected(Table table) throws Exception {
        try {
            table.addRow((Object) null);
        } catch (Exception error) {
            if (error.getMessage() != null && error.getMessage().contains("Invalid date/time literal")) {
                return;
            }
            throw error;
        }
        throw new AssertionError("Jackcess accepted an invalid date-only default");
    }

    public static void main(String[] args) throws Exception {
        Path path = Path.of(args[0]);
        Database.FileFormat format = Database.FileFormat.valueOf(args[1]);
        try (Database database = new DatabaseBuilder().withPath(path).withFileFormat(format).create()) {
            database.setEvaluateExpressions(true);
            Table table = new TableBuilder("DateDefaults")
                .addColumn(new ColumnBuilder("DateOnly", DataType.SHORT_DATE_TIME)
                    .withProperty(PropertyMap.DEFAULT_VALUE_PROP, "#1/2/2020#"))
                .addColumn(new ColumnBuilder("LeapDay", DataType.SHORT_DATE_TIME)
                    .withProperty(PropertyMap.DEFAULT_VALUE_PROP, "#2/29/2020#"))
                .addColumn(new ColumnBuilder("EarlyDate", DataType.SHORT_DATE_TIME)
                    .withProperty(PropertyMap.DEFAULT_VALUE_PROP, "#1/1/0100#"))
                .toTable(database);
            table.addRow((Object) null, null, null);
            table.addRow(LocalDateTime.of(2024, 12, 31, 23, 59, 59), null, null);
            Row first = table.iterator().next();
            if (!LocalDateTime.of(2020, 1, 2, 0, 0).equals(first.get("DateOnly")) ||
                    !LocalDateTime.of(2020, 2, 29, 0, 0).equals(first.get("LeapDay")) ||
                    !LocalDateTime.of(100, 1, 1, 0, 0).equals(first.get("EarlyDate"))) {
                throw new AssertionError("Jackcess date-only defaults returned unexpected values");
            }

            Table invalid = new TableBuilder("InvalidDateDefault")
                .addColumn(new ColumnBuilder("Value", DataType.SHORT_DATE_TIME)
                    .withProperty(PropertyMap.DEFAULT_VALUE_PROP, "#13/1/2020#"))
                .toTable(database);
            expectRejected(invalid);
        }
        System.out.println("Jackcess 5.2.0 date-only defaults passed for " + format);
    }
}
