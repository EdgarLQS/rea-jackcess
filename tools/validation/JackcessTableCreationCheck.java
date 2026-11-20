import io.github.spannm.jackcess.Column;
import io.github.spannm.jackcess.DataType;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Table;

import java.nio.file.Paths;
import java.util.Iterator;
import java.util.List;

/** Opens C++-created Jet4 tables with the pinned Jackcess 5.2.0 reference. */
public final class JackcessTableCreationCheck {
    private JackcessTableCreationCheck() {}

    public static void main(String[] args) throws Exception {
        if (args.length < 2) {
            throw new IllegalArgumentException(
                "Usage: JackcessTableCreationCheck <database.mdb> <Table=Column[,Column...]>...");
        }

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).withReadOnly(true).open()) {
            for (int argument = 1; argument < args.length; ++argument) {
                String[] tableAndColumns = args[argument].split("=", 2);
                if (tableAndColumns.length != 2 || tableAndColumns[0].isEmpty()) {
                    throw new IllegalArgumentException("Invalid table schema argument: " + args[argument]);
                }

                String tableName = tableAndColumns[0];
                if (!database.getTableNames().contains(tableName)) {
                    throw new AssertionError("Jackcess did not discover table " + tableName);
                }
                Table table = database.getTable(tableName);
                if (table == null) {
                    throw new AssertionError("Jackcess could not open table " + tableName);
                }

                String[] expectedColumns = tableAndColumns[1].split(",", -1);
                List<? extends Column> actualColumns = table.getColumns();
                if (actualColumns.size() != expectedColumns.length) {
                    throw new AssertionError("Unexpected column count for table " + tableName);
                }
                for (int columnIndex = 0; columnIndex < expectedColumns.length; ++columnIndex) {
                    Column column = actualColumns.get(columnIndex);
                    if (!column.getName().equals(expectedColumns[columnIndex]) || column.getType() != DataType.LONG) {
                        throw new AssertionError("Unexpected column schema in table " + tableName);
                    }
                }

                Iterator<?> rows = table.iterator();
                if (rows.hasNext()) {
                    throw new AssertionError("Newly created table unexpectedly contains rows: " + tableName);
                }
                System.out.println("Jackcess 5.2.0 opened " + tableName + " with " +
                                   actualColumns.size() + " LONG column(s) and no rows");
            }
        }
    }
}
