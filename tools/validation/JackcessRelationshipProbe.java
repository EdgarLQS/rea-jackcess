import io.github.spannm.jackcess.ColumnBuilder;
import io.github.spannm.jackcess.DataType;
import io.github.spannm.jackcess.Database;
import io.github.spannm.jackcess.DatabaseBuilder;
import io.github.spannm.jackcess.Relationship;
import io.github.spannm.jackcess.RelationshipBuilder;
import io.github.spannm.jackcess.TableBuilder;
import java.nio.file.Path;

public final class JackcessRelationshipProbe {
    public static void main(String[] args) throws Exception {
        Path path = Path.of(args[0]);
        try (var db = new DatabaseBuilder().withPath(path)
                .withFileFormat(Database.FileFormat.valueOf(args[1])).create()) {
            new TableBuilder("Parent")
                .addColumn(new ColumnBuilder("Region", DataType.LONG))
                .addColumn(new ColumnBuilder("Code", DataType.LONG))
                .withPrimaryKey("Region", "Code")
                .toTable(db);
            new TableBuilder("Child")
                .addColumn(new ColumnBuilder("ForeignRegion", DataType.LONG))
                .addColumn(new ColumnBuilder("ForeignCode", DataType.LONG))
                .toTable(db);
            new RelationshipBuilder("Parent", "Child")
                .withName("ParentChild")
                .addColumns("Region", "ForeignRegion")
                .addColumns("Code", "ForeignCode")
                .withReferentialIntegrity().withCascadeUpdates().withCascadeDeletes()
                .withJoinType(Relationship.JoinType.LEFT_OUTER)
                .toRelationship(db);
            new RelationshipBuilder("Parent", "Child")
                .withName("ReadOnlyJoin")
                .addColumns("Code", "ForeignCode")
                .addColumns("Region", "ForeignRegion")
                .addColumns("Code", "ForeignCode")
                .withJoinType(Relationship.JoinType.RIGHT_OUTER)
                .toRelationship(db);
        }
        try (var db = new DatabaseBuilder().withPath(path).withReadOnly(true).open()) {
            var relations = db.getRelationships();
            var parentChild = relations.stream().filter(rel -> rel.getName().equals("ParentChild"))
                .findFirst().orElseThrow();
            var join = relations.stream().filter(rel -> rel.getName().equals("ReadOnlyJoin"))
                .findFirst().orElseThrow();
            if (relations.size() != 2 ||
                !parentChild.getFromColumns().get(1).getName().equals("Code") ||
                !parentChild.getToColumns().get(1).getName().equals("ForeignCode") ||
                !parentChild.cascadeUpdates() || !parentChild.cascadeDeletes() ||
                parentChild.getJoinType() != Relationship.JoinType.LEFT_OUTER ||
                join.hasReferentialIntegrity() || join.getFromColumns().size() != 3 ||
                join.getJoinType() != Relationship.JoinType.RIGHT_OUTER) {
                throw new AssertionError(relations);
            }
        }
    }
}
