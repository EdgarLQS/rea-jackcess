import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Index;
import io.github.spannm.jackcess.Table;

import java.nio.file.Paths;
import java.util.List;

/** Verifies that Jackcess 5.2.0 reads a user index created by the C++ table builder. */
public final class JackcessIndexSchemaCheck {
    private JackcessIndexSchemaCheck() {}

    public static void main(String[] args) throws Exception {
        if (args.length != 3) {
            throw new IllegalArgumentException(
                "Usage: JackcessIndexSchemaCheck <database.mdb> <table> <index>");
        }
        String tableName = args[1];
        String indexName = args[2];

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).withReadOnly(true).open()) {
            Table table = database.getTable(tableName);
            if (table == null) {
                throw new AssertionError("Jackcess did not open table " + tableName);
            }
            List<? extends Index> indexes = table.getIndexes();
            Index found = null;
            for (Index index : indexes) {
                if (indexName.equals(index.getName())) {
                    found = index;
                }
            }
            if (found == null) {
                throw new AssertionError("Jackcess did not expose index " + indexName);
            }
            if (found.getColumns().size() != 1 || !"Id".equals(found.getColumns().get(0).getName())
                    || found.getColumns().get(0).isAscending() == false) {
                throw new AssertionError("Jackcess observed the wrong index columns");
            }
            if (found.isUnique() || found.isPrimaryKey() || found.isRequired() || found.shouldIgnoreNulls()) {
                throw new AssertionError("Jackcess observed unexpected index flags");
            }
            if (table.getDefaultCursor().getNextRow() != null) {
                throw new AssertionError("Jackcess observed rows through the default cursor");
            }
            if (table.getRowCount() != 0) {
                throw new AssertionError("Jackcess observed rows in a newly created table");
            }
        }
        System.out.println("Jackcess 5.2.0 read the created index schema and its empty cursor");
    }
}
