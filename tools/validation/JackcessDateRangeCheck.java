import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.io.File;
import java.time.ZoneId;
import java.util.Arrays;
import java.util.List;

public final class JackcessDateRangeCheck {
    private static final List<String> EXPECTED_DATES = Arrays.asList(
        "1765-06-14T12:45",
        "0100-06-14T12:45",
        "1899-12-30T00:00",
        "2024-03-04T05:06:07"
    );

    public static void main(String[] args) throws Exception {
        try (Database database = new DatabaseBuilder(new File(args[0])).withReadOnly(true).open()) {
            database.setZoneId(ZoneId.of("UTC"));
            Table table = database.getTable("Dated");
            int rowCount = 0;
            for (Row row : table) {
                String value = row.getLocalDateTime("When").toString();
                if (rowCount >= EXPECTED_DATES.size() || !EXPECTED_DATES.get(rowCount).equals(value)) {
                    throw new AssertionError("unexpected SHORT_DATE_TIME at row " + (rowCount + 1) + ": " + value);
                }
                System.out.println(row.getString("Label") + "|" + value);
                rowCount++;
            }
            if (rowCount != EXPECTED_DATES.size()) {
                throw new AssertionError("unexpected row count " + rowCount);
            }
        }
    }
}
