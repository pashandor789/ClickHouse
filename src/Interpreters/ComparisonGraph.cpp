#include <Interpreters/ComparisonGraph.h>
#include <Parsers/IAST.h>
#include <Parsers/ASTFunction.h>

#include <Poco/Logger.h>
#include <algorithm>
#include <deque>

namespace DB
{

/// make function a < b or a <= b
ASTPtr ComparisonGraph::normalizeAtom(const ASTPtr & atom) const
{
    static const std::map<std::string, std::string> inverse_relations = {
        {"greaterOrEquals", "less"},
        {"greater", "lessOrEquals"},
    };

    ASTPtr res = atom->clone();
    {
        auto * func = res->as<ASTFunction>();
        if (func)
        {
            if (const auto it = inverse_relations.find(func->name); it != std::end(inverse_relations))
            {
                res = makeASTFunction(it->second, func->arguments->children[1]->clone(), func->arguments->children[0]->clone());
            }
        }
    }

    return res;
}

ComparisonGraph::ComparisonGraph(const std::vector<ASTPtr> & atomic_formulas)
{
    static const std::map<std::string, Edge::Type> relation_to_enum = {
        {"equals", Edge::Type::EQUAL},
        {"less", Edge::Type::LESS},
        {"lessOrEquals", Edge::Type::LESS_OR_EQUAL},
    };

    Graph g;
    for (const auto & atom_raw : atomic_formulas) {
        const auto atom = normalizeAtom(atom_raw);

        const auto bad_term = std::numeric_limits<std::size_t>::max();
        auto get_index = [](const ASTPtr & ast, Graph & asts_graph) -> std::size_t {
            const auto it = asts_graph.ast_hash_to_component.find(ast->getTreeHash());
            if (it != std::end(asts_graph.ast_hash_to_component))
            {
                if (!std::any_of(
                        std::cbegin(asts_graph.vertexes[it->second].asts),
                        std::cend(asts_graph.vertexes[it->second].asts),
                        [ast](const ASTPtr & constraint_ast) {
                            return constraint_ast->getTreeHash() == ast->getTreeHash()
                                && constraint_ast->getColumnName() == ast->getColumnName();
                        }))
                {
                    return bad_term;
                }

                return it->second;
            }
            else
            {
                asts_graph.ast_hash_to_component[ast->getTreeHash()] = asts_graph.vertexes.size();
                asts_graph.vertexes.push_back(EqualComponent{{ast}});
                asts_graph.edges.emplace_back();
                return asts_graph.vertexes.size() - 1;
            }
        };

        const auto * func = atom->as<ASTFunction>();
        if (func)
        {
            if (const auto it = relation_to_enum.find(func->name); it != std::end(relation_to_enum) && func->arguments->children.size() == 2)
            {
                const size_t index_left = get_index(func->arguments->children[0], g);
                const size_t index_right = get_index(func->arguments->children[1], g);

                if (index_left != bad_term && index_right != bad_term)
                {
                    Poco::Logger::get("Edges").information("GOOD: " + atom->dumpTree());
                    Poco::Logger::get("Edges").information("left=" + std::to_string(index_left) + " right=" + std::to_string(index_right));
                    Poco::Logger::get("Edges").information("sz=" + std::to_string(g.edges.size()));
                    g.edges[index_right].push_back(Edge{it->second, index_left});
                    if (func->name == "equals")
                    {
                        Poco::Logger::get("Edges").information("right=" + std::to_string(index_left) + " left=" + std::to_string(index_right));
                        g.edges[index_left].push_back(Edge{it->second, index_right});
                    }
                }
                else
                {
                    Poco::Logger::get("Edges").information("BAD: " + atom->dumpTree());
                }
            }
        }
    }

    graph = BuildGraphFromAstsGraph(g);
}

std::pair<bool, bool> ComparisonGraph::findPath(const size_t start, const size_t finish) const
{
    // min path : < = -1, =< = 0
    const auto inf = std::numeric_limits<int64_t>::max();
    const size_t n = graph.vertexes.size();
    std::vector<int64_t> dist(n, inf);
    dist[start] = 0;
    for (size_t k = 0; k < n; ++k)
    {
        bool has_relaxation = false;
        for (size_t v = 0; v < n; ++v)
        {
            if (dist[v] == inf)
                continue;

            for (const auto & edge : graph.edges[v])
            {
                const int64_t weight = edge.type == Edge::Type::LESS ? -1 : 0;
                if (dist[edge.to] > dist[v] + weight)
                {
                    dist[edge.to] = dist[v] + weight;
                    has_relaxation = true;
                }
            }
        }

        if (has_relaxation)
            break;
    }
    return {dist[finish] != inf, dist[finish] < 0};
}

ComparisonGraph::CompareResult ComparisonGraph::compare(const ASTPtr & left, const ASTPtr & right) const
{
    size_t start = 0;
    size_t finish = 0;
    {
        /// TODO: check full ast
        const auto it_left = graph.ast_hash_to_component.find(left->getTreeHash());
        const auto it_right = graph.ast_hash_to_component.find(right->getTreeHash());
        if (it_left == std::end(graph.ast_hash_to_component) || it_right == std::end(graph.ast_hash_to_component))
        {
            Poco::Logger::get("Graph").information("not found");
            Poco::Logger::get("Graph").information(std::to_string(left->getTreeHash().second));
            Poco::Logger::get("Graph").information(std::to_string(right->getTreeHash().second));
            for (const auto & [hash, id] : graph.ast_hash_to_component)
            {
                Poco::Logger::get("Graph MAP").information(std::to_string(hash.second) + " "  + std::to_string(id));
            }
            return CompareResult::UNKNOWN;
        }
        else
        {
            start = it_left->second;
            finish = it_right->second;
            Poco::Logger::get("Graph").information("found:" + std::to_string(start) + " " + std::to_string(finish));
        }
    }

    if (start == finish)
        return CompareResult::EQUAL;

    /// TODO: precalculate using Floyd–Warshall O(n^3) algorithm where < = -1 and =< = 0.
    /// TODO: use it for less, greater and so on

    auto [has_path, is_strict] = findPath(start, finish);
    if (has_path)
        return is_strict ? CompareResult::GREATER : CompareResult::GREATER_OR_EQUAL;

    auto [has_path_r, is_strict_r] = findPath(finish, start);
    if (has_path_r)
        return is_strict_r ? CompareResult::LESS : CompareResult::LESS_OR_EQUAL;

    return CompareResult::UNKNOWN;
}

std::vector<ASTPtr> ComparisonGraph::getEqual(const ASTPtr & ast) const
{
    const auto hash_it = graph.ast_hash_to_component.find(ast->getTreeHash());
    if (hash_it != std::end(graph.ast_hash_to_component))
        return {};
    const size_t index = hash_it->second;
    if (std::any_of(
            std::cbegin(graph.vertexes[index].asts),
            std::cend(graph.vertexes[index].asts),
            [ast](const ASTPtr & constraint_ast)
            {
                return constraint_ast->getTreeHash() == ast->getTreeHash() &&
                    constraint_ast->getColumnName() == ast->getColumnName();
            })) {
        return graph.vertexes[index].asts;
    } else {
        return {};
    }
}

void ComparisonGraph::dfsOrder(const Graph & asts_graph, size_t v, std::vector<bool> & visited, std::vector<size_t> & order) const
{
    visited[v] = true;
    for (const auto & edge : asts_graph.edges[v])
    {
        if (!visited[edge.to])
        {
            dfsOrder(asts_graph, edge.to, visited, order);
        }
    }
    order.push_back(v);
}

ComparisonGraph::Graph ComparisonGraph::reverseGraph(const Graph & asts_graph) const
{
    Graph g;
    g.ast_hash_to_component = asts_graph.ast_hash_to_component;
    g.vertexes = asts_graph.vertexes;
    g.edges.resize(g.vertexes.size());
    for (size_t v = 0; v < asts_graph.vertexes.size(); ++v)
    {
        for (const auto & edge : asts_graph.edges[v])
        {
            g.edges[edge.to].push_back(Edge{edge.type, v});
        }
    }
    return asts_graph;
}

void ComparisonGraph::dfsComponents(
    const Graph & reversed_graph, size_t v, std::vector<size_t> & components, const size_t not_visited, const size_t component) const
{
    components[v] = component;
    for (const auto & edge : reversed_graph.edges[v])
    {
        if (components[edge.to] == not_visited)
        {
            dfsComponents(reversed_graph, edge.to, components, not_visited, component);
        }
    }
}

ComparisonGraph::Graph ComparisonGraph::BuildGraphFromAstsGraph(const Graph & asts_graph) const
{
    Poco::Logger::get("Graph").information("building");
    /// Find strongly connected component
    const auto n = asts_graph.vertexes.size();

    std::vector<size_t> order;
    {
        std::vector<bool> visited(n, false);
        for (size_t v = 0; v < n; ++v)
        {
            if (!visited[v])
                dfsOrder(asts_graph, v, visited, order);
        }
    }

    Poco::Logger::get("Graph").information("dfs1");

    const auto not_visited = std::numeric_limits<size_t>::max();
    std::vector<size_t> components(n, not_visited);
    size_t component = 0;
    {
        const Graph reversed_graph = reverseGraph(asts_graph);
        for (const size_t v : order)
        {
            if (components[v] == not_visited)
            {
                dfsComponents(reversed_graph, v, components, not_visited, component);
                ++component;
            }
        }
    }

    Poco::Logger::get("Graph").information("dfs2");

    Graph result;
    result.vertexes.resize(component);
    result.edges.resize(component);
    for (const auto & [hash, index] : asts_graph.ast_hash_to_component)
    {
        result.ast_hash_to_component[hash] = components[index];
        result.vertexes[components[index]].asts.insert(
            std::end(result.vertexes[components[index]].asts),
            std::begin(asts_graph.vertexes[index].asts),
            std::end(asts_graph.vertexes[index].asts)); // asts_graph has only one ast per vertex
    }

    Poco::Logger::get("Graph").information("components: " + std::to_string(component));

    for (size_t v = 0; v < n; ++v)
    {
        for (const auto & edge : asts_graph.edges[v])
        {
            result.edges[components[v]].push_back(Edge{edge.type, components[edge.to]});
        }
        // TODO: make edges unique (most strict)
    }

    Poco::Logger::get("Graph").information("finish");

    for (size_t v = 0; v < result.vertexes.size(); ++v)
    {
        std::stringstream s;
        for (const auto & atom : result.vertexes[v].asts)
        {
            s << atom->getTreeHash().second << " ";
        }
        s << "|";
        for (const auto & atom : result.ast_hash_to_component)
        {
            s << atom.first.second << " -" << atom.second << " ";
        }

        Poco::Logger::get("Graph").information(s.str());
    }

    return result;
}

}
