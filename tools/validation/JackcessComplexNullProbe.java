import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Row;
import io.github.spannm.jackcess.Table;

import java.io.IOException;
import java.nio.file.CopyOption;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardCopyOption;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Probes whether the fixed Jackcess 5.2.0 JAR can write a NULL complex-value foreign key.
 * <p>
 * The CSV gap recorded for IO-01 is that the committed ACE sample has no NULL complex cell. This
 * probe answers whether the reference implementation can create one: it copies the sample and
 * appends a row twice, once omitting the complex columns and once passing an explicit null, then
 * reports the stored foreign keys. Both attempts show a generated foreign key, so a NULL complex
 * cell cannot be produced through the public API and needs an Access-authored sample.
 * <p>
 * Usage: JackcessComplexNullProbe <complex-data-v2007.accdb> <scratch.accdb>
 */
public final class JackcessComplexNullProbe {

    private static final String[] COMPLEX_COLUMNS = {
        "VersionHistory_F5F8918F-0A3F-4DA9-AE71-184EE5012880",
        "multi-value-data",
        "attach-data",
    };

    private JackcessComplexNullProbe() {
    }

    public static void main(String[] args) throws IOException {
        if (args.length != 2) {
            System.err.println("usage: JackcessComplexNullProbe <source.accdb> <scratch.accdb>");
            System.exit(2);
        }
        final Path source = Paths.get(args[0]);
        final Path scratch = Paths.get(args[1]);
        Files.copy(source, scratch, new CopyOption[] {StandardCopyOption.REPLACE_EXISTING});

        try (Database database = DatabaseBuilder.open(scratch.toFile())) {
            final Table table = database.getTable("Table1");
            table.addRowFromMap(omitted());
            table.addRowFromMap(explicitNull());
        }

        try (Database database = DatabaseBuilder.open(scratch.toFile())) {
            final Table table = database.getTable("Table1");
            table.forEach(row -> {
                final StringBuilder line = new StringBuilder();
                line.append("row id=").append(row.get("id")).append(" complex values:");
                for (String column : COMPLEX_COLUMNS) {
                    line.append(' ').append(column).append('=').append(row.get(column));
                }
                System.out.println(line);
            });
        }
        System.out.println("Both rows carry a generated foreign key, so Jackcess cannot write a NULL "
            + "complex cell; the NULL CSV case needs an Access-authored sample.");
    }

    private static Map<String, Object> omitted() {
        final Map<String, Object> values = new LinkedHashMap<>();
        values.put("id", "row-omitted");
        values.put("memo-data", "complex columns omitted");
        return values;
    }

    private static Map<String, Object> explicitNull() {
        final Map<String, Object> values = omitted();
        values.put("id", "row-explicit-null");
        for (String column : COMPLEX_COLUMNS) {
            values.put(column, null);
        }
        return values;
    }
}
