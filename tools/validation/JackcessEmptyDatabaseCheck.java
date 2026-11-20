import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;

import java.nio.file.Paths;

/** Verifies a C++-created empty MDB with the pinned Jackcess reference implementation. */
public final class JackcessEmptyDatabaseCheck {
    public static void main(String[] args) throws Exception {
        if (args.length != 2) {
            throw new IllegalArgumentException("usage: JackcessEmptyDatabaseCheck <database-file> <V2000|V2003>");
        }

        try (Database database = new DatabaseBuilder().withPath(Paths.get(args[0])).withReadOnly(true).open()) {
            final String actualFormat = database.getFileFormat().name();
            if (!actualFormat.equals(args[1])) {
                throw new AssertionError("expected format " + args[1] + " but got " + actualFormat);
            }
            if (!database.getTableNames().isEmpty()) {
                throw new AssertionError("new database contains user tables: " + database.getTableNames());
            }
            System.out.println("Jackcess read-back passed: format=" + actualFormat + " userTables=0");
        }
    }
}
