import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Relationship;
import java.nio.file.Path;

public final class JackcessRelationshipBothFlagsCheck {
    public static void main(String[] args) throws Exception {
        try (var db = new DatabaseBuilder().withPath(Path.of(args[0])).withReadOnly(true).open()) {
            var relationships = db.getRelationships();
            var relation = relationships.stream()
                .filter(item -> item.getName().equals("DeleteAndNull"))
                .findFirst().orElseThrow();
            if (!relation.cascadeDeletes() || !relation.cascadeNullOnDelete()) {
                throw new AssertionError("combined delete action flags were not preserved");
            }
            var manyColumns = relationships.stream()
                .filter(item -> item.getName().equals("ManyColumnJoin"))
                .findFirst().orElseThrow();
            if (relationships.size() != 2 || manyColumns.hasReferentialIntegrity() ||
                manyColumns.getFromColumns().size() != 11 || manyColumns.getToColumns().size() != 11) {
                throw new AssertionError("unenforced 11-column relationship was not preserved");
            }
            System.out.println("Jackcess 5.2.0 read combined delete flags and an unenforced 11-column relationship");
        }
    }
}
