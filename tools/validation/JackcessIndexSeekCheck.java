import io.github.spannm.jackcess.Cursor;
import io.github.spannm.jackcess.CursorBuilder;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;

/**
 * Checks that Jackcess 5.2.0 agrees with the C++ seek and range semantics on an index: a seek to
 * a key must return exactly the suffix of the full index scan from that key, an exact lookup must
 * return the first row of that suffix, and [start, stop) must be the slice in between.
 *
 * Usage: JackcessIndexSeekCheck <database.mdb> <table> <index> <integer column> <start> <stop>
 */
public final class JackcessIndexSeekCheck {
    private JackcessIndexSeekCheck() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 7) {
            throw new IllegalArgumentException(
                "Usage: JackcessIndexSeekCheck <database.mdb> <table> <index> <column> <start> <stop>");
        }
        String tableName = args[1];
        String indexName = args[2];
        String columnName = args[3];
        int start = Integer.parseInt(args[4]);
        int stop = Integer.parseInt(args[5]);
        int expectedStartCount = Integer.parseInt(args[6]);

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).withReadOnly(true).open()) {
            Table table = database.getTable(tableName);
            if (table == null) {
                throw new AssertionError("Jackcess did not open table " + tableName);
            }

            List<Integer> all = new ArrayList<>();
            Cursor full = new CursorBuilder(table).withIndexByName(indexName).toCursor();
            Row row;
            while ((row = full.getNextRow()) != null) {
                all.add(((Number) row.get(columnName)).intValue());
            }

            List<Integer> seek = new ArrayList<>();
            Cursor fromStart = new CursorBuilder(table).withIndexByName(indexName)
                .withStartRow(start).withStartRowInclusive(true).toCursor();
            while ((row = fromStart.getNextRow()) != null) {
                seek.add(((Number) row.get(columnName)).intValue());
            }
            List<Integer> expectedSuffix = new ArrayList<>();
            for (int value : all) {
                if (value >= start) {
                    expectedSuffix.add(value);
                }
            }
            if (!seek.equals(expectedSuffix)) {
                throw new AssertionError("seek to " + start + " returned " + seek.size()
                    + " keys instead of the " + expectedSuffix.size() + "-key suffix");
            }
            if (seek.size() != expectedStartCount) {
                throw new AssertionError("seek to " + start + " returned " + seek.size()
                    + " keys, expected " + expectedStartCount);
            }

            Cursor exact = new CursorBuilder(table).withIndexByName(indexName)
                .withSpecificRow(start).toCursor();
            Row hit = exact.getNextRow();
            boolean present = all.contains(start);
            if (present != (hit != null)) {
                throw new AssertionError("exact lookup of " + start + " disagreed with the scan");
            }
            if (hit != null && ((Number) hit.get(columnName)).intValue() != seek.get(0)) {
                throw new AssertionError("exact lookup of " + start + " did not return the first entry");
            }

            List<Integer> range = new ArrayList<>();
            Cursor ranged = new CursorBuilder(table).withIndexByName(indexName)
                .withStartRow(start).withStartRowInclusive(true)
                .withEndRow(stop).withEndRowInclusive(false).toCursor();
            while ((row = ranged.getNextRow()) != null) {
                range.add(((Number) row.get(columnName)).intValue());
            }
            List<Integer> expectedRange = new ArrayList<>();
            for (int value : all) {
                if (value >= start && value < stop) {
                    expectedRange.add(value);
                }
            }
            if (!range.equals(expectedRange)) {
                throw new AssertionError("range [" + start + "," + stop + ") returned " + range.size()
                    + " keys instead of " + expectedRange.size());
            }

            System.out.println("Jackcess 5.2.0 matched the seek/range semantics: seek " + start + " -> "
                + seek.size() + " keys, exact=" + (hit != null) + ", range [" + start + "," + stop
                + ") -> " + range.size() + " keys");
        }
    }
}
