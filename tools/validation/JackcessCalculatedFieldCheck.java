import io.github.spannm.jackcess.Column;
import io.github.spannm.jackcess.DataType;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.PropertyMap;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.io.File;
import java.util.Arrays;
import java.util.List;

public final class JackcessCalculatedFieldCheck {
    private static final List<String> CALCULATED_COLUMNS = Arrays.asList(
        "LastFirst", "LastFirstLen", "MonthlySalary", "IsRich", "AllNames", "WeeklySalary",
        "SalaryTest", "BoolTest", "DecimalTest", "FloatTest", "BigNumTest"
    );

    private static final List<DataType> RESULT_TYPES = Arrays.asList(
        DataType.TEXT, DataType.LONG, DataType.MONEY, DataType.BOOLEAN, DataType.MEMO, DataType.NUMERIC,
        DataType.MONEY, DataType.BOOLEAN, DataType.NUMERIC, DataType.FLOAT, DataType.NUMERIC
    );

    private static final List<String> EXPECTED_ROWS = Arrays.asList(
        "Wayne, Bruce|12|83333.3333|true|Wayne, Bruce=Wayne, Bruce|19230.7692307692|1000000.0000|true|50.325000|2583.2092|56505085819.424791296572280180",
        "Simpson, Bart|13|-0.0833|false|Simpson, Bart=Simpson, Bart|-0.0192307692307692|-1.0000|true|-36.222200|0.0035889593|-0.0784734499180612994241100748",
        "Doe, John|9|0.0000|false|Doe, John=Doe, John|0|0.0000|true|0.012300|0.0|0E-8",
        "User, Test|10|8.3333|false|User, Test=User, Test|1.92307692307692|100.0000|true|102030405060.654321|1.27413E-10|2.787019289824216980830E-7"
    );

    public static void main(String[] args) throws Exception {
        try (Database database = new DatabaseBuilder(new File(args[0])).withReadOnly(true).open()) {
            Table table = database.getTable("Table1");
            for (int index = 0; index < CALCULATED_COLUMNS.size(); index++) {
                Column column = table.getColumn(CALCULATED_COLUMNS.get(index));
                Object resultType = column.getProperties().getValue(PropertyMap.RESULT_TYPE_PROP);
                if (!column.isCalculated() || column.getType() != RESULT_TYPES.get(index) ||
                    !Byte.valueOf((byte) RESULT_TYPES.get(index).getValue()).equals(resultType)) {
                    throw new AssertionError("calculated type mismatch for " + column.getName());
                }
            }

            int rowNumber = 0;
            for (Row row : table) {
                if (rowNumber >= EXPECTED_ROWS.size()) {
                    throw new AssertionError("calculated-field fixture returned extra rows");
                }
                StringBuilder actual = new StringBuilder();
                for (String columnName : CALCULATED_COLUMNS) {
                    if (actual.length() > 0) {
                        actual.append('|');
                    }
                    Object value = row.get(columnName);
                    actual.append(value == null ? "null" : value.toString());
                }
                if (!EXPECTED_ROWS.get(rowNumber).equals(actual.toString())) {
                    throw new AssertionError("calculated values differ at row " + (rowNumber + 1) + ": " + actual);
                }
                System.out.println((rowNumber + 1) + "|" + actual);
                rowNumber++;
            }
            if (rowNumber != EXPECTED_ROWS.size()) {
                throw new AssertionError("calculated-field fixture returned " + rowNumber + " rows");
            }
        }
    }
}
