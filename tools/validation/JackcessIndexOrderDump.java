import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Index;
import io.github.spannm.jackcess.IndexCursor;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;
import io.github.spannm.jackcess.impl.RowIdImpl;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

/** Dumps per-index row-id sequences for comparison with the C++ validation probe. */
public final class JackcessIndexOrderDump {
    public static void main(String[] args) throws Exception {
        if (args.length != 1) {
            throw new IllegalArgumentException("usage: JackcessIndexOrderDump <database-file>");
        }

        Map<String, List<String>> sequences = new TreeMap<>();
        try (Database database = new DatabaseBuilder(new File(args[0])).withReadOnly(true).open()) {
            for (String tableName : database.getTableNames()) {
                Table table = database.getTable(tableName);
                if (table.isSystem()) {
                    continue;
                }
                for (Index index : table.getIndexes()) {
                    IndexCursor cursor = index.newCursor().toIndexCursor();
                    List<String> sequence = sequences.computeIfAbsent(
                            tableName + "\t" + index.getName(), ignored -> new ArrayList<>());
                    Row row;
                    while ((row = cursor.getNextRow()) != null) {
                        RowIdImpl id = (RowIdImpl) row.getId();
                        sequence.add(id.getPageNumber() + ":" + id.getRowNumber());
                    }
                }
            }
        }

        for (Map.Entry<String, List<String>> entry : sequences.entrySet()) {
            System.out.println(entry.getKey() + "\t" + String.join(",", entry.getValue()));
        }
    }
}
