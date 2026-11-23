import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.io.File;
import java.time.ZoneId;
import java.util.Arrays;
import java.util.List;

public final class JackcessExtendedDateCheck {
    private static final List<String> EXPECTED_DATES = Arrays.asList(
        "2020-06-17T00:00",
        "2021-06-14T00:00",
        "2021-06-14T12:45",
        "2021-06-14T01:45",
        null,
        "2021-06-14T22:45:12.345678900",
        "1765-06-14T12:45",
        "0100-06-14T12:45:00.123456700",
        "1265-06-14T12:45"
    );

    private static final List<String> EXPECTED_TEXT = Arrays.asList(
        "6/17/2020",
        "6/14/2021",
        "6/14/2021 12:45:00.0000000 PM",
        "6/14/2021 1:45:00.0000000 AM",
        null,
        "6/14/2021 10:45:12.3456789 PM",
        "6/14/1765 12:45:00.0000000 PM",
        "6/14/100 12:45:00.1234567 PM",
        "6/14/1265 12:45:00.0000000 PM"
    );

    private static final List<String> EXPECTED_CALCULATED_DATES = Arrays.asList(
        "2020-06-17T00:00",
        "2021-06-14T00:00",
        "2021-06-14T12:45",
        "2021-06-14T01:45",
        null,
        "2021-06-14T22:45:12",
        "1765-06-14T12:45",
        "0100-06-14T12:45",
        "1265-06-14T12:45"
    );

    public static void main(String[] args) throws Exception {
        try (Database database = new DatabaseBuilder(new File(args[0])).withReadOnly(true).open()) {
            database.setZoneId(ZoneId.of("America/New_York"));
            Table table = database.getTable("Table1");
            int rowCount = 0;
            for (Row row : table) {
                if (rowCount >= EXPECTED_DATES.size()) {
                    throw new AssertionError("V2019 fixture returned extra rows");
                }
                String date = row.getLocalDateTime("DateExt") == null
                    ? null
                    : row.getLocalDateTime("DateExt").toString();
                String calculatedDate = row.getLocalDateTime("DateNormalCalc") == null
                    ? null
                    : row.getLocalDateTime("DateNormalCalc").toString();
                String text = row.getString("DateExtStr");
                if (!equalNullable(EXPECTED_DATES.get(rowCount), date) ||
                    !equalNullable(EXPECTED_TEXT.get(rowCount), text) ||
                    !equalNullable(EXPECTED_CALCULATED_DATES.get(rowCount), calculatedDate)) {
                    throw new AssertionError("unexpected dates at row " + (rowCount + 1) + ": " + date +
                        " | " + text + " | " + calculatedDate);
                }
                System.out.println((rowCount + 1) + "|" + date + "|" + text + "|" + calculatedDate);
                rowCount++;
            }
            if (rowCount != EXPECTED_DATES.size()) {
                throw new AssertionError("V2019 fixture returned " + rowCount + " rows");
            }
        }
    }

    private static boolean equalNullable(String expected, String actual) {
        return expected == null ? actual == null : expected.equals(actual);
    }
}
