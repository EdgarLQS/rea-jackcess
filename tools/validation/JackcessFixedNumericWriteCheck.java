import io.github.spannm.jackcess.Column;
import io.github.spannm.jackcess.DataType;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.nio.file.Paths;
import java.util.Iterator;
import java.util.List;

/** Verifies C++ fixed-width numeric row writes with Jackcess 5.2.0. */
public final class JackcessFixedNumericWriteCheck {
    private JackcessFixedNumericWriteCheck() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 1) {
            throw new IllegalArgumentException("Usage: JackcessFixedNumericWriteCheck <database.mdb>");
        }

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).withReadOnly(true).open()) {
            Table table = database.getTable("T");
            if (table == null || table.getColumns().size() != 5 || table.getRowCount() != 3) {
                throw new AssertionError("Jackcess could not find the expected three-row numeric table T");
            }
            List<? extends Column> columns = table.getColumns();
            DataType[] expectedTypes = {DataType.BYTE, DataType.INT, DataType.LONG, DataType.FLOAT, DataType.DOUBLE};
            String[] expectedNames = {"ByteValue", "ShortValue", "LongValue", "SingleValue", "DoubleValue"};
            for (int index = 0; index < expectedTypes.length; ++index) {
                if (!expectedNames[index].equals(columns.get(index).getName()) ||
                    columns.get(index).getType() != expectedTypes[index]) {
                    throw new AssertionError("Jackcess observed the wrong fixed numeric schema");
                }
            }

            Iterator<Row> rows = table.iterator();
            if (!rows.hasNext()) {
                throw new AssertionError("Jackcess did not find the lower-boundary numeric row");
            }
            Row lower = rows.next();
            if (((Number) lower.get("ByteValue")).byteValue() != -128 ||
                ((Number) lower.get("ShortValue")).shortValue() != Short.MIN_VALUE ||
                ((Number) lower.get("LongValue")).intValue() != Integer.MIN_VALUE ||
                Float.compare(((Number) lower.get("SingleValue")).floatValue(), -1.25F) != 0 ||
                Double.compare(((Number) lower.get("DoubleValue")).doubleValue(), 1.0 / 3.0) != 0) {
                throw new AssertionError("Jackcess did not observe the lower-boundary numeric values");
            }

            if (!rows.hasNext()) {
                throw new AssertionError("Jackcess did not find the upper-boundary numeric row");
            }
            Row upper = rows.next();
            if (((Number) upper.get("ByteValue")).byteValue() != 127 ||
                ((Number) upper.get("ShortValue")).shortValue() != 32767 ||
                ((Number) upper.get("LongValue")).intValue() != Integer.MAX_VALUE ||
                Float.compare(((Number) upper.get("SingleValue")).floatValue(), 3.5F) != 0 ||
                Double.compare(((Number) upper.get("DoubleValue")).doubleValue(), 2.5) != 0) {
                throw new AssertionError("Jackcess did not observe the upper-boundary numeric row");
            }

            if (!rows.hasNext()) {
                throw new AssertionError("Jackcess did not find the updated numeric row");
            }
            Row updated = rows.next();
            if (((Number) updated.get("ByteValue")).byteValue() != -1 ||
                ((Number) updated.get("ShortValue")).shortValue() != 0 ||
                ((Number) updated.get("LongValue")).intValue() != 123456 ||
                Float.compare(((Number) updated.get("SingleValue")).floatValue(), 12.5F) != 0 ||
                Double.compare(((Number) updated.get("DoubleValue")).doubleValue(), -0.5) != 0 || rows.hasNext()) {
                throw new AssertionError("Jackcess did not observe the fixed-width numeric row update");
            }
        }

        System.out.println("Jackcess 5.2.0 verified BYTE, SHORT, LONG, SINGLE, and DOUBLE row writes");
    }
}
